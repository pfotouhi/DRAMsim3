#include "controller.h"
#include <iomanip>
#include <iostream>
#include <limits>

namespace dramsim3 {

#ifdef THERMAL
Controller::Controller(int channel, const Config &config, const Timing &timing,
                       ThermalCalculator &thermal_calc)
#else
Controller::Controller(int channel, const Config &config, const Timing &timing)
#endif  // THERMAL
    : channel_id_(channel),
      clk_(0),
      config_(config),
      simple_stats_(config_, channel_id_),
      channel_state_(config, timing),
      cmd_queue_(channel_id_, config, channel_state_, simple_stats_),
      refresh_(config, channel_state_),
#ifdef THERMAL
      thermal_calc_(thermal_calc),
#endif  // THERMAL
      is_unified_queue_(config.unified_queue),
      is_dist_controller_(config.dist_controller),
      last_unified_requester_(0),
      last_read_requester_(0),
      last_write_requester_(0),
      row_buf_policy_(config.row_buf_policy == "CLOSE_PAGE"
                          ? RowBufPolicy::CLOSE_PAGE
                          : RowBufPolicy::OPEN_PAGE),
      last_trans_clk_(0),
      write_draining_(0) {
    if (is_dist_controller_) {
        if (is_unified_queue_) {
	    // Per requester queues
	    dist_unified_queue_.reserve(config_.requesters_per_channel);
            for (auto i = 0; i < config_.requesters_per_channel; i++) {
                dist_unified_queue_[i].reserve(config_.dist_trans_queue_size);
            }
	    // Single entry queue where one request form different dist_queues
	    // are added to
            unified_queue_.reserve(1);
        } else {
	    // Per requester queues
            dist_read_queue_.reserve(config_.requesters_per_channel);
            dist_write_buffer_.reserve(config_.requesters_per_channel);
            for (auto i = 0; i < config_.requesters_per_channel; i++) {
                dist_read_queue_[i].reserve(config_.dist_trans_queue_size);
                dist_write_buffer_[i].reserve(config_.dist_trans_queue_size);
            }
	    // Single entry read queue per bank where one request form 
	    // different dist_queues are added to
	    auto banks_per_channel = config_.bankgroups * config_.banks_per_group;
            per_bank_read_queue_.reserve(banks_per_channel);
            for (auto i = 0; i < banks_per_channel; i++) {
            	per_bank_read_queue_[i].reserve(1);
            }
            write_buffer_.reserve(32);
        }
    } else  {
        if (is_unified_queue_) {
            unified_queue_.reserve(config_.trans_queue_size);
        } else {
            read_queue_.reserve(config_.trans_queue_size);
            write_buffer_.reserve(config_.trans_queue_size);
        }
    }

#ifdef CMD_TRACE
    std::string trace_file_name = config_.output_prefix + "ch_" +
                                  std::to_string(channel_id_) + "cmd.trace";
    std::cout << "Command Trace write to " << trace_file_name << std::endl;
    cmd_trace_.open(trace_file_name, std::ofstream::out);
#endif  // CMD_TRACE
}

std::pair<uint64_t, int> Controller::ReturnDoneTrans(uint64_t clk) {
    auto it = return_queue_.begin();
    while (it != return_queue_.end()) {
	if (is_dist_controller_) {
	    if (clk >= it->complete_cycle + config_.link_latency) {
                if (it->is_write) {
                    simple_stats_.Increment("num_writes_done");
                } else {
                    simple_stats_.Increment("num_reads_done");
                    simple_stats_.AddValue("read_latency", clk_ - it->added_cycle);
                    simple_stats_.AddValue("total_read_latency", clk_ - it->start_cycle);
                }
                auto pair = std::make_pair(it->addr, it->is_write);
                it = return_queue_.erase(it);
                return pair;
            } else {
                ++it;
	    }
	} else {
            if (clk >= it->complete_cycle) {
                if (it->is_write) {
                    simple_stats_.Increment("num_writes_done");
                } else {
                    simple_stats_.Increment("num_reads_done");
                    simple_stats_.AddValue("read_latency", clk_ - it->added_cycle);
                    simple_stats_.AddValue("total_read_latency", clk_ - it->start_cycle);
                }
                auto pair = std::make_pair(it->addr, it->is_write);
                it = return_queue_.erase(it);
                return pair;
            } else {
                ++it;
            }
	}
    }
    return std::make_pair(-1, -1);
}

void Controller::ClockTick() {
    // update refresh counter
    refresh_.ClockTick();

    bool cmd_issued = false;
    Command cmd;
    if (channel_state_.IsRefreshWaiting()) {
        cmd = cmd_queue_.FinishRefresh();
    }

    // cannot find a refresh related command or there's no refresh
    if (!cmd.IsValid()) {
        cmd = cmd_queue_.GetCommandToIssue();
    }

    if (cmd.IsValid()) {
        IssueCommand(cmd);
        cmd_issued = true;

        if (config_.enable_hbm_dual_cmd) {
            auto second_cmd = cmd_queue_.GetCommandToIssue();
            if (second_cmd.IsValid()) {
                if (second_cmd.IsReadWrite() != cmd.IsReadWrite()) {
                    IssueCommand(second_cmd);
                    simple_stats_.Increment("hbm_dual_cmds");
                }
            }
        }
    }

    // power updates pt 1
    for (int i = 0; i < config_.ranks; i++) {
        if (channel_state_.IsRankSelfRefreshing(i)) {
            simple_stats_.IncrementVec("sref_cycles", i);
        } else {
            bool all_idle = channel_state_.IsAllBankIdleInRank(i);
            if (all_idle) {
                simple_stats_.IncrementVec("all_bank_idle_cycles", i);
                channel_state_.rank_idle_cycles[i] += 1;
            } else {
                simple_stats_.IncrementVec("rank_active_cycles", i);
                // reset
                channel_state_.rank_idle_cycles[i] = 0;
            }
        }
    }

    // power updates pt 2: move idle ranks into self-refresh mode to save power
    if (config_.enable_self_refresh && !cmd_issued) {
        for (auto i = 0; i < config_.ranks; i++) {
            if (channel_state_.IsRankSelfRefreshing(i)) {
                // wake up!
                if (!cmd_queue_.rank_q_empty[i]) {
                    auto addr = Address();
                    addr.rank = i;
                    auto cmd = Command(CommandType::SREF_EXIT, addr, -1);
                    cmd = channel_state_.GetReadyCommand(cmd, clk_);
                    if (cmd.IsValid()) {
                        IssueCommand(cmd);
                        break;
                    }
                }
            } else {
                if (cmd_queue_.rank_q_empty[i] &&
                    channel_state_.rank_idle_cycles[i] >=
                        config_.sref_threshold) {
                    auto addr = Address();
                    addr.rank = i;
                    auto cmd = Command(CommandType::SREF_ENTER, addr, -1);
                    cmd = channel_state_.GetReadyCommand(cmd, clk_);
                    if (cmd.IsValid()) {
                        IssueCommand(cmd);
                        break;
                    }
                }
            }
        }
    }

    if (is_dist_controller_)
        QueueIn();
    ScheduleTransaction();
    clk_++;
    cmd_queue_.ClockTick();
    simple_stats_.Increment("num_cycles");
    return;
}

bool Controller::WillAcceptTransaction(uint64_t hex_addr, bool is_write) const {
    if (is_unified_queue_) {
        return unified_queue_.size() < unified_queue_.capacity();
    } else if (!is_write) {
        return read_queue_.size() < read_queue_.capacity();
    } else {
        return write_buffer_.size() < write_buffer_.capacity();
    }
}

bool Controller::WillAcceptTransaction(uint64_t hex_addr, uint64_t requester, bool is_write) const {
    // We should only call this with distriuted memory controllers
    assert(is_dist_controller_);
    if (is_unified_queue_) {
        return dist_unified_queue_[requester].size() < dist_unified_queue_[requester].capacity();
    } else if (!is_write) {
        return dist_read_queue_[requester].size() < dist_read_queue_[requester].capacity();
    } else {
        return dist_write_buffer_[requester].size() < dist_write_buffer_[requester].capacity();
    }
}

bool Controller::AddTransaction(Transaction trans) {
    if (is_dist_controller_) {
        trans.added_cycle = clk_;
        simple_stats_.AddValue("interarrival_latency", clk_ - last_trans_clk_);
        simple_stats_.AddValue("stall_latency", clk_ - trans.start_cycle);
        last_trans_clk_ = clk_;

	// Here we only add elements to queues, the rest of functionality
	// for distributed memory controllers is moved to QueueIn
        if (trans.is_write) {
            simple_stats_.AddValue("write_stall_latency", clk_ - trans.start_cycle);
            if (is_unified_queue_) {
                dist_unified_queue_[trans.requester].push_back(trans);
            } else {
                dist_write_buffer_[trans.requester].push_back(trans);
            }
            return true;
        } else {  // read
            simple_stats_.AddValue("read_stall_latency", clk_ - trans.start_cycle);
            if (is_unified_queue_) {
                dist_unified_queue_[trans.requester].push_back(trans);
            } else {
                dist_read_queue_[trans.requester].push_back(trans);
            }
            return true;
        }
    } else {
        trans.added_cycle = clk_;
        simple_stats_.AddValue("interarrival_latency", clk_ - last_trans_clk_);
        simple_stats_.AddValue("stall_latency", clk_ - trans.start_cycle);
        last_trans_clk_ = clk_;

        if (trans.is_write) {
            simple_stats_.AddValue("write_stall_latency", clk_ - trans.start_cycle);
            if (pending_wr_q_.count(trans.addr) == 0) {  // can not merge writes
                pending_wr_q_.insert(std::make_pair(trans.addr, trans));
                if (is_unified_queue_) {
                    unified_queue_.push_back(trans);
                } else {
                    write_buffer_.push_back(trans);
                }
            }
            trans.complete_cycle = clk_ + 1;
            return_queue_.push_back(trans);
            return true;
        } else {  // read
            simple_stats_.AddValue("read_stall_latency", clk_ - trans.start_cycle);
            // if in write buffer, use the write buffer value
            if (pending_wr_q_.count(trans.addr) > 0) {
                trans.complete_cycle = clk_ + 1;
                return_queue_.push_back(trans);
		// ADD read_queue_lat and read_command queue lat
                return true;
            }
            pending_rd_q_.insert(std::make_pair(trans.addr, trans));
            if (pending_rd_q_.count(trans.addr) == 1) {
                if (is_unified_queue_) {
                    unified_queue_.push_back(trans);
                } else {
                    read_queue_.push_back(trans);
                }
            }
            return true;
        }
    }
}

void Controller::QueueIn() {
    assert(is_dist_controller_);
    Transaction trans;
    bool write_done_ = false;
    bool read_done_ = false;
    // Make sure we will always have at most 1 request at shared queue
    if (is_unified_queue_) {
	if (unified_queue_.size() >= 1)
	    return;
    } else {
	if (write_buffer_.size() >= 32) {
	    write_done_ = true;
	}
	// Assume all per bank queues are full
	read_done_ = true;
	auto banks_per_channel = config_.bankgroups * config_.banks_per_group;
        for (auto i = 0; i < banks_per_channel; i++) {
	    // Check if there is at least one that is not full
	    if (per_bank_read_queue_[i].size() < 1) {
	        read_done_ = false;
		break;
	    }
        }
	if (write_done_ and read_done_)
	    return;
    }

    for (auto i = 0; i < config_.requesters_per_channel; i++) {
        if (is_unified_queue_) {
	    // Start iterating through requesters from the  one after last requester
	    uint64_t requester_ = (last_unified_requester_ + 1 + i) 
		    % config_.requesters_per_channel;
	    if (dist_unified_queue_[requester_].empty()) // skip this requester
		continue;
	    trans = dist_unified_queue_[requester_].front();
	    trans.dist_link_start = clk_;
            if (trans.is_write) {
                if (pending_wr_q_.count(trans.addr) == 0) {  // can not merge writes
                    pending_wr_q_.insert(std::make_pair(trans.addr, trans));
                    unified_queue_.push_back(trans);
                }
                trans.complete_cycle = clk_ + 1;
                return_queue_.push_back(trans);
            } else {  // read
                // if in write buffer, use the write buffer value
                if (pending_wr_q_.count(trans.addr) > 0) {
                    trans.complete_cycle = clk_ + 1;
                    return_queue_.push_back(trans);
                }
                pending_rd_q_.insert(std::make_pair(trans.addr, trans));
                if (pending_rd_q_.count(trans.addr) == 1)
                   unified_queue_.push_back(trans);
            }
	    dist_unified_queue_[requester_].erase(dist_unified_queue_[requester_].begin());
	    last_unified_requester_ = requester_;
	    break;
        } else {
	    // Starting with writes
	    // Start iterating through requesters from the one after last requester
	    uint64_t write_requester_ = (last_write_requester_ + 1 + i) 
		    % config_.requesters_per_channel;
	    if (!write_done_ && !dist_write_buffer_[write_requester_].empty()) {
	        trans = dist_write_buffer_[write_requester_].front();
		trans.dist_link_start = clk_;
                if (pending_wr_q_.count(trans.addr) == 0) {  // can not merge writes
                    pending_wr_q_.insert(std::make_pair(trans.addr, trans));
                    write_buffer_.push_back(trans);
                }
                trans.complete_cycle = clk_ + 1;
                return_queue_.push_back(trans);
	        dist_write_buffer_[write_requester_].erase(dist_write_buffer_[write_requester_].begin());
	        last_write_requester_ = write_requester_;
		write_done_ = true;
	    }
	    // Now reads 
	    // Start iterating through requesters from the one after last requester
	    uint64_t read_requester_ = (last_read_requester_ + 1 + i) 
		    % config_.requesters_per_channel;
	    if (!read_done_ && !dist_read_queue_[read_requester_].empty()) {
	        trans = dist_read_queue_[read_requester_].front();
                // if in write buffer, use the write buffer value
                if (pending_wr_q_.count(trans.addr) > 0) {
                    trans.complete_cycle = clk_ + 1;
		    // ADD read_queue_lat and read_command queue lat
                    return_queue_.push_back(trans);
                } else {
    		    auto read_addr = config_.AddressMapping(trans.addr);
		    auto queue_id = (read_addr.bankgroup * config_.banks_per_group) + read_addr.bank;
		    // Check if per bank queue for this request is full
	            if (per_bank_read_queue_[queue_id].size() >= 1)
		        continue;
		    trans.dist_link_start = clk_;
                    pending_rd_q_.insert(std::make_pair(trans.addr, trans));
                    if (pending_rd_q_.count(trans.addr) == 1)
                        per_bank_read_queue_[queue_id].push_back(trans);
		}
	        dist_read_queue_[read_requester_].erase(dist_read_queue_[read_requester_].begin());
	        last_read_requester_ = read_requester_;
		read_done_ = true;
	    }
	    // Done if we have issued one read and one write
	    if (write_done_ and read_done_)
	        break;
	}
    } 
}


void Controller::ScheduleTransaction() {
    // determine whether to schedule read or write
    if (write_draining_ == 0 && !is_unified_queue_) {
        // we basically have a upper and lower threshold for write buffer
        if ((write_buffer_.size() >= write_buffer_.capacity()) ||
            (write_buffer_.size() > 8 && cmd_queue_.QueueEmpty())) {
            write_draining_ = write_buffer_.size();
        }
    }

    if (is_dist_controller_) {
        if (write_draining_ > 0) {
	    std::vector<Transaction> &queue = write_buffer_;
	    for (auto it = queue.begin(); it != queue.end(); it++) {
	        // for distributed memory controller design we should consider
	        // the inerconnect latency.
	        if (it->dist_link_start + config_.link_latency > clk_)
	        	continue;
	        auto cmd = TransToCommand(*it);
	        if (cmd_queue_.WillAcceptCommand(cmd.Rank(), cmd.Bankgroup(),
	                                         cmd.Bank())) {
	    
	            // update stats for all pending requests
	            assert(cmd.iswrite());
	            auto num_requests = pending_wr_q_.count(cmd.hex_addr);
		    assert(num_requests == 1); // if this does not fail remove the whole things
	            while (num_requests > 0) {
	                auto req = pending_wr_q_.find(cmd.hex_addr);
	                req->second.schedule_cycle = clk_;
	                simple_stats_.AddValue("command_queuing_latency", 
	            	req->second.schedule_cycle - req->second.added_cycle);
	                simple_stats_.AddValue("write_command_queuing_latency", 
	            	req->second.schedule_cycle - req->second.added_cycle);
	                num_requests -= 1;
	            }
	    
	            // enforce r->w dependency
	            if (pending_rd_q_.count(it->addr) > 0) {
	                write_draining_ = 0;
	                break;
	            }
	            write_draining_ -= 1;
	            cmd_queue_.AddCommand(cmd);
	            queue.erase(it);
	            break;
	        }
	    }
	} else {
	    auto banks_per_channel = config_.bankgroups * config_.banks_per_group;
	    // For reads in distributed MC, instead of iterating over all elements in a 
	    // single read queue, we check signle entry queues per bank
            for (auto i = 0; i < banks_per_channel; i++) {
	        // Check if queue is empty
	        if (per_bank_read_queue_[i].size() < 1) 
	            continue;
		// Sanity check
		assert(per_bank_read_queue_[i] == 1);
	        std::vector<Transaction> &queue = per_bank_read_queue_[i];
		auto it = queue.begin();
	        // for distributed memory controller design we should consider
	        // the inerconnect latency.
	        if (it->dist_link_start + config_.link_latency > clk_)
	            continue;
	        auto cmd = TransToCommand(*it);
	        if (cmd_queue_.WillAcceptCommand(cmd.Rank(), cmd.Bankgroup(),
	                                         cmd.Bank())) {
	    
	            // update stats for all pending requests
	            assert(!cmd.iswrite());
	            auto num_requests = pending_rd_q_.count(cmd.hex_addr);
	            while (num_requests > 0) {
	                auto req = pending_rd_q_.find(cmd.hex_addr);
	                req->second.schedule_cycle = clk_;
	                simple_stats_.AddValue("command_queuing_latency", 
	            	req->second.schedule_cycle - req->second.added_cycle);
	                simple_stats_.AddValue("read_command_queuing_latency", 
	            	req->second.schedule_cycle - req->second.added_cycle);
	                num_requests -= 1;
	            }
	    
	            cmd_queue_.AddCommand(cmd);
	            queue.erase(it);
	            break;
	        }
	    }
	}
    } else {
	std::vector<Transaction> &queue =
	    is_unified_queue_ ? unified_queue_
	                      : write_draining_ > 0 ? write_buffer_ : read_queue_;
	for (auto it = queue.begin(); it != queue.end(); it++) {
	    auto cmd = TransToCommand(*it);
	    if (cmd_queue_.WillAcceptCommand(cmd.Rank(), cmd.Bankgroup(),
	                                     cmd.Bank())) {
	
	        // update stats for all pending requests
	        if (cmd.IsWrite()) {
	            auto num_requests = pending_wr_q_.count(cmd.hex_addr);
	            while (num_requests > 0) {
	                auto req = pending_wr_q_.find(cmd.hex_addr);
	                req->second.schedule_cycle = clk_;
	    	    simple_stats_.AddValue("command_queuing_latency", 
	        		req->second.schedule_cycle - req->second.added_cycle);
	    	    simple_stats_.AddValue("write_command_queuing_latency", 
	        		req->second.schedule_cycle - req->second.added_cycle);
	                num_requests -= 1;
	    	    }
	        } else {
	            auto num_requests = pending_rd_q_.count(cmd.hex_addr);
	            while (num_requests > 0) {
	                auto req = pending_rd_q_.find(cmd.hex_addr);
	                req->second.schedule_cycle = clk_;
	    	    simple_stats_.AddValue("command_queuing_latency", 
	        		req->second.schedule_cycle - req->second.added_cycle);
	    	    simple_stats_.AddValue("read_command_queuing_latency", 
	        		req->second.schedule_cycle - req->second.added_cycle);
	                num_requests -= 1;
	            }
	        }
	
	        if (!is_unified_queue_ && cmd.IsWrite()) {
	            // Enforce R->W dependency
	            if (pending_rd_q_.count(it->addr) > 0) {
	                write_draining_ = 0;
	                break;
	            }
	            write_draining_ -= 1;
	        }
	        cmd_queue_.AddCommand(cmd);
	        queue.erase(it);
	        break;
	    }
	}
    }
}

void Controller::IssueCommand(const Command &cmd) {
#ifdef CMD_TRACE
    cmd_trace_ << std::left << std::setw(18) << clk_ << " " << cmd << std::endl;
#endif  // CMD_TRACE
#ifdef THERMAL
    // add channel in, only needed by thermal module
    thermal_calc_.UpdateCMDPower(channel_id_, cmd, clk_);
#endif  // THERMAL
    // if read/write, update pending queue and return queue
    if (cmd.IsRead()) {
        auto num_reads = pending_rd_q_.count(cmd.hex_addr);
        if (num_reads == 0) {
            std::cerr << cmd.hex_addr << " not in read queue! " << std::endl;
            exit(1);
        }
        // if there are multiple reads pending return them all
        while (num_reads > 0) {
            auto it = pending_rd_q_.find(cmd.hex_addr);
            it->second.issue_cycle = clk_;
    	    simple_stats_.AddValue("queuing_latency", 
		it->second.issue_cycle - it->second.schedule_cycle);
    	    simple_stats_.AddValue("read_queuing_latency", 
		it->second.issue_cycle - it->second.schedule_cycle);
	    // sanity check to make sure we are recording schedule_cycle
	    assert(it->second.schedule_cycle != 0);
            it->second.complete_cycle = clk_ + config_.read_delay;
            return_queue_.push_back(it->second);
            pending_rd_q_.erase(it);
            num_reads -= 1;
        }
    } else if (cmd.IsWrite()) {
        // there should be only 1 write to the same location at a time
        auto it = pending_wr_q_.find(cmd.hex_addr);
        if (it == pending_wr_q_.end()) {
            std::cerr << cmd.hex_addr << " not in write queue!" << std::endl;
            exit(1);
        }
        it->second.issue_cycle = clk_;
    	simple_stats_.AddValue("queuing_latency", 
		it->second.issue_cycle - it->second.schedule_cycle);
    	simple_stats_.AddValue("write_queuing_latency", 
		it->second.issue_cycle - it->second.schedule_cycle);
	// sanity check to make sure we are recording schedule_cycle
	assert(it->second.schedule_cycle != 0);
        auto wr_lat = clk_ - it->second.added_cycle + config_.write_delay;
        simple_stats_.AddValue("write_latency", wr_lat);
        auto wr_total_lat = clk_ - it->second.start_cycle + config_.write_delay;
        simple_stats_.AddValue("total_write_latency", wr_total_lat);
        pending_wr_q_.erase(it);
    }
    // must update stats before states (for row hits)
    UpdateCommandStats(cmd);
    channel_state_.UpdateTimingAndStates(cmd, clk_);
}

Command Controller::TransToCommand(const Transaction &trans) {
    auto addr = config_.AddressMapping(trans.addr);
    CommandType cmd_type;
    if (row_buf_policy_ == RowBufPolicy::OPEN_PAGE) {
        cmd_type = trans.is_write ? CommandType::WRITE : CommandType::READ;
    } else {
        cmd_type = trans.is_write ? CommandType::WRITE_PRECHARGE
                                  : CommandType::READ_PRECHARGE;
    }
    return Command(cmd_type, addr, trans.addr);
}

int Controller::QueueUsage() const { return cmd_queue_.QueueUsage(); }

void Controller::PrintEpochStats() {
    simple_stats_.Increment("epoch_num");
    simple_stats_.PrintEpochStats();
#ifdef THERMAL
    for (int r = 0; r < config_.ranks; r++) {
        double bg_energy = simple_stats_.RankBackgroundEnergy(r);
        thermal_calc_.UpdateBackgroundEnergy(channel_id_, r, bg_energy);
    }
#endif  // THERMAL
    return;
}

void Controller::PrintFinalStats() {
    simple_stats_.PrintFinalStats();

#ifdef THERMAL
    for (int r = 0; r < config_.ranks; r++) {
        double bg_energy = simple_stats_.RankBackgroundEnergy(r);
        thermal_calc_.UpdateBackgroundEnergy(channel_id_, r, bg_energy);
    }
#endif  // THERMAL
    return;
}

void Controller::UpdateCommandStats(const Command &cmd) {
    switch (cmd.cmd_type) {
        case CommandType::READ:
        case CommandType::READ_PRECHARGE:
            simple_stats_.Increment("num_read_cmds");
            if (channel_state_.RowHitCount(cmd.Rank(), cmd.Bankgroup(),
                                           cmd.Bank()) != 0) {
                simple_stats_.Increment("num_read_row_hits");
            }
            break;
        case CommandType::WRITE:
        case CommandType::WRITE_PRECHARGE:
            simple_stats_.Increment("num_write_cmds");
            if (channel_state_.RowHitCount(cmd.Rank(), cmd.Bankgroup(),
                                           cmd.Bank()) != 0) {
                simple_stats_.Increment("num_write_row_hits");
            }
            break;
        case CommandType::ACTIVATE:
            simple_stats_.Increment("num_act_cmds");
            break;
        case CommandType::PRECHARGE:
            simple_stats_.Increment("num_pre_cmds");
            break;
        case CommandType::REFRESH:
            simple_stats_.Increment("num_ref_cmds");
            break;
        case CommandType::REFRESH_BANK:
            simple_stats_.Increment("num_refb_cmds");
            break;
        case CommandType::SREF_ENTER:
            simple_stats_.Increment("num_srefe_cmds");
            break;
        case CommandType::SREF_EXIT:
            simple_stats_.Increment("num_srefx_cmds");
            break;
        default:
            AbruptExit(__FILE__, __LINE__);
    }
}

}  // namespace dramsim3
