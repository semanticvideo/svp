#include "svp/exec/worker_session_leases.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/task_frames.hpp"

#include <utility>

namespace svp::exec {

void WorkerSessionLeases::add(std::string lease_id, std::string task_id,
                              std::uint64_t attempt) {
  leases_.insert_or_assign(std::move(lease_id),
                           Outstanding{.task_id = std::move(task_id), .attempt = attempt});
}

bool WorkerSessionLeases::contains(std::string_view lease_id) const {
  return leases_.contains(lease_id);
}

bool WorkerSessionLeases::mark_cancelled(std::string_view lease_id) {
  const auto found = leases_.find(lease_id);
  if (found == leases_.end()) {
    return false;
  }
  found->second.cancelled = true;
  return true;
}

bool WorkerSessionLeases::is_live(std::string_view lease_id) const {
  const auto found = leases_.find(lease_id);
  return found != leases_.end() && !found->second.cancelled;
}

std::optional<std::string> WorkerSessionLeases::claim_result(const TaskResult& result) {
  for (auto iterator = leases_.begin(); iterator != leases_.end(); ++iterator) {
    if (iterator->second.task_id == result.task_id &&
        iterator->second.attempt == result.attempt) {
      std::optional<std::string> lease_id;
      if (!iterator->second.cancelled) {
        lease_id = iterator->first;
      }
      leases_.erase(iterator);
      return lease_id;
    }
  }
  throw ExecError(ExecErrorCode::frame_malformed,
                  "RESULT for task `" + result.task_id + "` attempt " +
                      std::to_string(result.attempt) +
                      " matches no lease issued to this worker");
}

std::vector<std::string> WorkerSessionLeases::take_live() {
  std::vector<std::string> live;
  for (const auto& [lease_id, lease] : leases_) {
    if (!lease.cancelled) {
      live.push_back(lease_id);
    }
  }
  leases_.clear();
  return live;
}

namespace {

void handle_worker_frame(Frame frame, std::mutex& mutex, WorkerSessionLeases& leases,
                         ExecutorEvents& events) {
  switch (frame.type) {
    case MessageType::heartbeat: {
      const std::string lease_id = lease_id_from_heartbeat_frame(frame);
      bool live = false;
      {
        const std::lock_guard lock(mutex);
        live = leases.is_live(lease_id);
      }
      if (live) {
        events.lease_heartbeat(lease_id);
      }
      return;
    }
    case MessageType::result: {
      TaskResult result = task_result_from_result_frame(frame);
      std::optional<std::string> lease_id;
      {
        const std::lock_guard lock(mutex);
        lease_id = leases.claim_result(result);
      }
      if (lease_id) {
        events.attempt_finished(*lease_id,
                                AttemptOutput{.result = std::move(result),
                                              .payloads = std::move(frame.payloads)});
      }
      return;
    }
    default:
      throw ExecError(ExecErrorCode::frame_malformed,
                      "worker sent an unexpected " +
                          std::string(message_type_name(frame.type)) + " frame");
  }
}

}  // namespace

WorkerSessionEnd pump_worker_frames(FrameReader& reader, std::mutex& mutex,
                                    WorkerSessionLeases& leases, ExecutorEvents& events,
                                    std::string_view end_of_stream_reason) {
  WorkerSessionEnd end{.failure = AttemptFailureKind::executor_lost,
                       .reason = std::string(end_of_stream_reason)};
  try {
    while (auto frame = reader.read()) {
      if (frame->type == MessageType::error) {
        end.reason = "worker reported a protocol error: " + frame->body.dump();
        break;
      }
      handle_worker_frame(std::move(*frame), mutex, leases, events);
    }
  } catch (const ExecError& error) {
    // A stream that ends inside a frame is a worker that died mid-write, not
    // a worker that lied; everything else is bytes we cannot accept.
    if (error.code() != ExecErrorCode::frame_truncated) {
      end.failure = AttemptFailureKind::invalid_result;
    }
    end.reason = std::string("worker stream rejected: ") + error.what();
  }
  return end;
}

}  // namespace svp::exec
