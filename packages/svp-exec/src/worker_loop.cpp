#include "svp/exec/worker_loop.hpp"

#include "lease_heartbeats.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/task_attempt_runner.hpp"
#include "svp/exec/task_frames.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace svp::exec {
namespace {

struct RunningLease {
  bool dropped = false;
  // The attempt's token; set when the lease is dropped (CANCEL, SHUTDOWN, or
  // end of input) so the task function can stop at its next check.
  std::shared_ptr<CancellationToken> cancellation = std::make_shared<CancellationToken>();
};

// Everything the reader thread, task threads, and heartbeat thread share.
class WorkerSession {
 public:
  WorkerSession(FrameWriter& writer, const TaskTypeRegistry& registry,
                TaskArtifactAccess& artifacts, const WorkerLoopOptions& options)
      : writer_(writer),
        registry_(registry),
        artifacts_(artifacts),
        options_(options),
        heartbeats_([this](const std::string& lease_id) {
          writer_.write(make_heartbeat_frame(lease_id));
        }) {}

  WorkerSession(const WorkerSession&) = delete;
  WorkerSession& operator=(const WorkerSession&) = delete;

  ~WorkerSession() { close(); }

  // Returns false when lease_id is already running.
  bool start(LeasedAssignment assignment) {
    reap_finished();
    const std::lock_guard lock(mutex_);
    const std::string lease_id = assignment.lease.lease_id;
    if (leases_.contains(lease_id)) {
      return false;
    }
    RunningLease lease;
    std::shared_ptr<CancellationToken> cancellation = lease.cancellation;
    leases_.emplace(lease_id, std::move(lease));
    heartbeats_.add(lease_id, assignment.lease.heartbeat_interval);
    std::thread thread([this, assignment = std::move(assignment), cancellation] {
      run(assignment, *cancellation);
    });
    const std::thread::id thread_id = thread.get_id();
    threads_.emplace(thread_id, std::move(thread));
    return true;
  }

  void drop(const std::string& lease_id) {
    const std::lock_guard lock(mutex_);
    if (const auto found = leases_.find(lease_id); found != leases_.end()) {
      found->second.dropped = true;
      found->second.cancellation->request();
      heartbeats_.remove(lease_id);
    }
  }

  void send(const Frame& frame) { writer_.write(frame); }

  // Drops every lease, waits for task threads, and stops heartbeats.
  void close() {
    {
      const std::lock_guard lock(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
      for (auto& [lease_id, lease] : leases_) {
        lease.dropped = true;
        lease.cancellation->request();
        heartbeats_.remove(lease_id);
      }
    }
    std::map<std::thread::id, std::thread> threads;
    {
      const std::lock_guard lock(mutex_);
      threads.swap(threads_);
    }
    for (auto& [thread_id, thread] : threads) {
      thread.join();
    }
    heartbeats_.stop();
  }

 private:
  void run(const LeasedAssignment& assignment, const CancellationToken& cancellation) {
    AttemptOutput output = run_task_attempt(
        registry_, artifacts_, assignment.spec,
        AttemptContext{.attempt = assignment.lease.attempt,
                       .worker_session_id = options_.worker_session_id,
                       .runtime_id = options_.runtime_id},
        cancellation);
    std::optional<Frame> frame;
    try {
      frame = make_result_frame(output.result, std::move(output.payloads));
    } catch (const ExecError& error) {
      frame = make_result_frame(output_mismatch_failure(output.result, error), {});
    }
    heartbeats_.remove(assignment.lease.lease_id);
    bool dropped = false;
    {
      const std::lock_guard lock(mutex_);
      dropped = leases_.at(assignment.lease.lease_id).dropped;
    }
    if (!dropped) {
      try {
        writer_.write(*frame);
      } catch (const ExecError&) {
        // The coordinator is gone; the reader sees end of input and closes.
      }
    }
    const std::lock_guard lock(mutex_);
    leases_.erase(assignment.lease.lease_id);
    finished_.push_back(std::this_thread::get_id());
  }

  TaskResult output_mismatch_failure(const TaskResult& original, const ExecError& error) {
    TaskResult failed = original;
    failed.status = TaskStatus::failed;
    failed.outputs.clear();
    failed.output_digest = compute_output_digest({});
    failed.error = TaskError{.code = std::string(exec_error_code_name(error.code())),
                             .message = error.what(),
                             .retryable = false};
    return failed;
  }

  void reap_finished() {
    std::vector<std::thread> finished;
    {
      const std::lock_guard lock(mutex_);
      for (const std::thread::id thread_id : finished_) {
        if (const auto found = threads_.find(thread_id); found != threads_.end()) {
          finished.push_back(std::move(found->second));
          threads_.erase(found);
        }
      }
      finished_.clear();
    }
    for (std::thread& thread : finished) {
      thread.join();
    }
  }

  FrameWriter& writer_;
  const TaskTypeRegistry& registry_;
  TaskArtifactAccess& artifacts_;
  const WorkerLoopOptions& options_;

  std::mutex mutex_;
  std::map<std::string, RunningLease> leases_;
  std::map<std::thread::id, std::thread> threads_;
  std::vector<std::thread::id> finished_;
  bool closed_ = false;
  // Declared last: its thread calls writer_ and must stop first.
  detail::LeaseHeartbeats heartbeats_;
};

void send_protocol_error(WorkerSession& session, std::string_view message) {
  try {
    session.send(Frame{.type = MessageType::error,
                       .body = nlohmann::json{{"code", std::string(kWorkerProtocolErrorCode)},
                                              {"message", std::string(message)}},
                       .payloads = {}});
  } catch (const ExecError&) {
    // Output already closed; nothing more to report.
  }
}

}  // namespace

std::string_view worker_loop_exit_name(WorkerLoopExit exit) noexcept {
  switch (exit) {
    case WorkerLoopExit::input_closed:
      return "input_closed";
    case WorkerLoopExit::shutdown:
      return "shutdown";
    case WorkerLoopExit::protocol_error:
      return "protocol_error";
  }
  return "unknown";
}

WorkerLoopExit run_worker_loop(FrameReader& input, FrameWriter& output,
                               const TaskTypeRegistry& registry,
                               TaskArtifactAccess& artifacts,
                               const WorkerLoopOptions& options) {
  WorkerSession session(output, registry, artifacts, options);
  while (true) {
    std::optional<Frame> frame;
    try {
      frame = input.read();
    } catch (const ExecError& error) {
      send_protocol_error(session, error.what());
      return WorkerLoopExit::protocol_error;
    }
    if (!frame) {
      return WorkerLoopExit::input_closed;
    }
    try {
      switch (frame->type) {
        case MessageType::assign: {
          LeasedAssignment assignment = leased_assignment_from_frame(*frame);
          const std::string lease_id = assignment.lease.lease_id;
          if (!session.start(std::move(assignment))) {
            send_protocol_error(session, "lease `" + lease_id + "` is already running");
            return WorkerLoopExit::protocol_error;
          }
          break;
        }
        case MessageType::cancel:
          session.drop(lease_id_from_cancel_frame(*frame));
          break;
        case MessageType::shutdown:
          return WorkerLoopExit::shutdown;
        default:
          send_protocol_error(session, "unexpected " +
                                           std::string(message_type_name(frame->type)) +
                                           " frame");
          return WorkerLoopExit::protocol_error;
      }
    } catch (const ExecError& error) {
      send_protocol_error(session, error.what());
      return WorkerLoopExit::protocol_error;
    }
  }
}

WorkerLoopExit run_worker_loop(int in_fd, int out_fd, const TaskTypeRegistry& registry,
                               TaskArtifactAccess& artifacts,
                               const WorkerLoopOptions& options) {
  FdFrameReader reader(in_fd, options.frame_limits);
  FdFrameWriter writer(out_fd, options.frame_limits);
  return run_worker_loop(reader, writer, registry, artifacts, options);
}

}  // namespace svp::exec
