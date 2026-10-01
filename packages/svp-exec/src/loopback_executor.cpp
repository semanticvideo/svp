#include "svp/exec/loopback_executor.hpp"

#include "child_process.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/worker_session_leases.hpp"

#include <csignal>
#include <condition_variable>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace svp::exec {
namespace {

// One worker process and the thread that reads from it.
struct Session {
  detail::ChildProcess process;
  std::unique_ptr<FdFrameWriter> writer;
  std::thread reader;
  WorkerSessionLeases leases;
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

  void read_session(Session& session) {
    FdFrameReader reader(session.process.fd, options.frame_limits);
    const WorkerSessionEnd end =
        pump_worker_frames(reader, mutex, session.leases, *events, "worker process ended");
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
      orphaned = session.leases.take_live();
      session_finished.notify_all();
    }
    if (report) {
      for (const std::string& lease_id : orphaned) {
        events->attempt_failed(lease_id, end.failure, end.reason);
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
    session->leases.add(lease.lease_id, spec.task_id, lease.attempt);
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
    session->leases.mark_cancelled(lease_id);
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
  session->leases.mark_cancelled(lease_id);
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
