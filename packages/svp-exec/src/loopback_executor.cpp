#include "svp/exec/loopback_executor.hpp"

#include "child_process.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/task_frames.hpp"

#include <csignal>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace svp::exec {
namespace {

using AttemptKey = std::pair<std::string, std::uint64_t>;

struct OutstandingLease {
  AttemptKey key;
  // Cancelled leases stay known so a result already in flight is recognised
  // and ignored rather than treated as a result nobody asked for.
  bool cancelled = false;
};

// One worker process and the thread that reads from it.
struct Session {
  detail::ChildProcess process;
  std::unique_ptr<FdFrameWriter> writer;
  std::thread reader;
  std::map<std::string, OutstandingLease, std::less<>> leases;
  // Set (under the executor mutex) before the reader reaps the process; once
  // set, the pid may be reused and must never be signalled.
  bool reaping = false;
  // Killed on purpose (lease expiry); takes no new leases while it dies.
  bool retiring = false;
  bool finished = false;
};

// Caller holds the executor mutex.
void kill_session_process(const Session& session) {
  if (!session.reaping) {
    ::kill(session.process.pid, SIGKILL);
  }
}

}  // namespace

struct LoopbackExecutor::State {
  explicit State(LoopbackExecutorOptions options_in) : options(std::move(options_in)) {}

  // Caller holds the mutex.
  std::shared_ptr<Session> live_session() {
    if (current && !current->finished && !current->retiring) {
      return current;
    }
    auto session = std::make_shared<Session>();
    session->process =
        detail::spawn_child_process(options.worker_executable, options.worker_arguments);
    session->writer = std::make_unique<FdFrameWriter>(session->process.fd,
                                                      options.frame_limits);
    ++processes_started;
    if (current) {
      retired.push_back(current);
    }
    current = session;
    session->reader = std::thread([this, session] { read_session(*session); });
    return session;
  }

  std::shared_ptr<Session> session_for(std::string_view lease_id) {
    for (const auto& session : sessions()) {
      if (session->leases.contains(lease_id)) {
        return session;
      }
    }
    return nullptr;
  }

  std::vector<std::shared_ptr<Session>> sessions() const {
    std::vector<std::shared_ptr<Session>> all = retired;
    if (current) {
      all.push_back(current);
    }
    return all;
  }

  // Returns the lease to report a result for, or nullopt when the result
  // belongs to a cancelled lease. Throws when nobody asked for it.
  std::optional<std::string> claim_result(Session& session, const TaskResult& result) {
    const std::lock_guard lock(mutex);
    for (auto iterator = session.leases.begin(); iterator != session.leases.end();
         ++iterator) {
      if (iterator->second.key == AttemptKey{result.task_id, result.attempt}) {
        std::optional<std::string> lease_id;
        if (!iterator->second.cancelled) {
          lease_id = iterator->first;
        }
        session.leases.erase(iterator);
        return lease_id;
      }
    }
    throw ExecError(ExecErrorCode::frame_malformed,
                    "RESULT for task `" + result.task_id + "` attempt " +
                        std::to_string(result.attempt) +
                        " matches no lease issued to this worker");
  }

  bool has_live_lease(Session& session, const std::string& lease_id) {
    const std::lock_guard lock(mutex);
    const auto found = session.leases.find(lease_id);
    return found != session.leases.end() && !found->second.cancelled;
  }

  void handle_frame(Session& session, Frame frame) {
    switch (frame.type) {
      case MessageType::heartbeat: {
        const std::string lease_id = lease_id_from_heartbeat_frame(frame);
        if (has_live_lease(session, lease_id)) {
          events->lease_heartbeat(lease_id);
        }
        return;
      }
      case MessageType::result: {
        TaskResult result = task_result_from_result_frame(frame);
        if (const auto lease_id = claim_result(session, result)) {
          events->attempt_finished(
              *lease_id, AttemptOutput{.result = std::move(result),
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

  void read_session(Session& session) {
    FdFrameReader reader(session.process.fd, options.frame_limits);
    AttemptFailureKind failure = AttemptFailureKind::executor_lost;
    std::string reason = "worker process ended";
    try {
      while (auto frame = reader.read()) {
        if (frame->type == MessageType::error) {
          reason = "worker reported a protocol error: " + frame->body.dump();
          break;
        }
        handle_frame(session, std::move(*frame));
      }
    } catch (const ExecError& error) {
      // A stream that ends inside a frame is a process that died mid-write,
      // not a worker that lied; everything else is bytes we cannot accept.
      if (error.code() != ExecErrorCode::frame_truncated) {
        failure = AttemptFailureKind::invalid_result;
      }
      reason = std::string("worker stream rejected: ") + error.what();
    }
    {
      const std::lock_guard lock(mutex);
      session.reaping = true;
    }
    detail::kill_and_reap(session.process.pid);

    std::vector<std::string> orphaned;
    bool report = false;
    {
      const std::lock_guard lock(mutex);
      session.finished = true;
      report = !stopping;
      for (const auto& [lease_id, lease] : session.leases) {
        if (!lease.cancelled) {
          orphaned.push_back(lease_id);
        }
      }
      session.leases.clear();
      session_finished.notify_all();
    }
    if (report) {
      for (const std::string& lease_id : orphaned) {
        events->attempt_failed(lease_id, failure, reason);
      }
    }
  }

  void close_session(Session& session, std::unique_lock<std::mutex>& lock) {
    if (!session.finished) {
      lock.unlock();
      try {
        session.writer->write(make_shutdown_frame());
      } catch (const ExecError&) {
        // Already gone; the reader is finishing.
      }
      ::shutdown(session.process.fd, SHUT_WR);
      lock.lock();
      if (!session_finished.wait_for(lock, options.shutdown_grace,
                                     [&] { return session.finished; })) {
        kill_session_process(session);
      }
    }
    lock.unlock();
    if (session.reader.joinable()) {
      session.reader.join();
    }
    lock.lock();
    if (session.process.fd >= 0) {
      ::close(session.process.fd);
      session.process.fd = -1;
    }
  }

  LoopbackExecutorOptions options;
  ExecutorEvents* events = nullptr;
  mutable std::mutex mutex;
  std::condition_variable session_finished;
  std::shared_ptr<Session> current;
  std::vector<std::shared_ptr<Session>> retired;
  std::size_t processes_started = 0;
  bool stopping = false;
};

LoopbackExecutor::LoopbackExecutor(LoopbackExecutorOptions options)
    : state_(std::make_unique<State>(std::move(options))) {
  if (state_->options.slots == 0 || state_->options.worker_executable.empty()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "loopback executor needs a worker executable and at least one slot");
  }
}

LoopbackExecutor::~LoopbackExecutor() { stop(); }

std::string_view LoopbackExecutor::id() const { return state_->options.executor_id; }

std::size_t LoopbackExecutor::slots() const { return state_->options.slots; }

std::size_t LoopbackExecutor::processes_started() const {
  const std::lock_guard lock(state_->mutex);
  return state_->processes_started;
}

void LoopbackExecutor::start(ExecutorEvents& events) { state_->events = &events; }

void LoopbackExecutor::assign(const TaskSpec& spec, const Lease& lease) {
  State& state = *state_;
  std::shared_ptr<Session> session;
  try {
    const std::lock_guard lock(state.mutex);
    session = state.live_session();
    session->leases.emplace(lease.lease_id,
                            OutstandingLease{.key = {spec.task_id, lease.attempt}});
  } catch (const ExecError& error) {
    state.events->attempt_failed(lease.lease_id, AttemptFailureKind::executor_lost,
                                 error.what());
    return;
  }
  try {
    session->writer->write(make_leased_assign_frame(spec, lease));
  } catch (const ExecError&) {
    // The process is gone; its reader reports this lease as lost.
  }
}

void LoopbackExecutor::cancel(std::string_view lease_id) {
  State& state = *state_;
  std::shared_ptr<Session> session;
  {
    const std::lock_guard lock(state.mutex);
    session = state.session_for(lease_id);
    if (!session || session->finished) {
      return;
    }
    session->leases.find(lease_id)->second.cancelled = true;
  }
  try {
    session->writer->write(make_cancel_frame(lease_id));
  } catch (const ExecError&) {
    // The process is gone; nothing to cancel.
  }
}

void LoopbackExecutor::lease_expired(std::string_view lease_id) {
  State& state = *state_;
  const std::lock_guard lock(state.mutex);
  const std::shared_ptr<Session> session = state.session_for(lease_id);
  if (!session || session->finished) {
    return;
  }
  session->leases.find(lease_id)->second.cancelled = true;
  // The reader sees end of stream, reaps the process, and fails the rest.
  session->retiring = true;
  kill_session_process(*session);
}

void LoopbackExecutor::stop() {
  State& state = *state_;
  std::unique_lock lock(state.mutex);
  state.stopping = true;
  for (const std::shared_ptr<Session>& session : state.sessions()) {
    state.close_session(*session, lock);
  }
  state.retired.clear();
  state.current.reset();
}

}  // namespace svp::exec
