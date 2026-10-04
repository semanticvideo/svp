#include "svp/exec/remote/remote_executor.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/frame_stream.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/worker_session_leases.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace svp::exec::remote {
namespace {

struct PendingAssign {
  std::string lease_id;
  Frame frame;
};

// One connection to the worker and the thread that opens and reads it.
struct Session {
  std::shared_ptr<RemoteConnector> connector;
  // Set once, under the executor mutex, when the connection opens; writer
  // is declared after stream so it is destroyed first.
  std::unique_ptr<RemoteStream> stream;
  std::unique_ptr<StreamFrameWriter> writer;
  // ASSIGN frames issued before the connection opened.
  std::vector<PendingAssign> pending;
  std::thread thread;
  WorkerSessionLeases leases;
  // The connection is open (stream and writer are set).
  bool connected = false;
  // The preamble finished: ASSIGN frames go straight to the writer.
  bool ready = false;
  // Torn down on purpose (lease expiry); takes no new leases while it ends.
  bool retiring = false;
  bool finished = false;
};

}  // namespace

struct RemoteExecutor::State {
  explicit State(RemoteExecutorOptions options_in) : options(std::move(options_in)) {}

  // Caller holds the mutex.
  std::shared_ptr<Session> live_session() {
    if (current && !current->finished && !current->retiring) {
      return current;
    }
    auto session = std::make_shared<Session>();
    session->connector = std::make_shared<RemoteConnector>(options.connector);
    if (current) {
      retired.push_back(current);
    }
    current = session;
    session->thread = std::thread([this, session] { run_session(*session); });
    return session;
  }

  std::shared_ptr<Session> session_for(std::string_view lease_id) const {
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

  // Returns true when the session took the connection.
  bool adopt_connection(Session& session, RemoteConnection connection) {
    const std::lock_guard lock(mutex);
    if (session.retiring || stopping) {
      connection.stream->cancel();
      return false;
    }
    session.stream = std::move(connection.stream);
    session.writer = std::make_unique<StreamFrameWriter>(*session.stream, options.frame_limits);
    session.connected = true;
    ++connections_opened;
    last_route = std::move(connection.route);
    return true;
  }

  // Marks the session ready and moves its queued ASSIGNs to `pending`.
  void mark_ready(Session& session, std::vector<PendingAssign>& pending) {
    const std::lock_guard lock(mutex);
    session.ready = true;
    pending.swap(session.pending);
  }

  // Connects, rediscovering the worker until reconnect_window runs out.
  std::optional<RemoteConnection> connect_within_window(Session& session, std::string& reason) {
    const auto give_up = std::chrono::steady_clock::now() + options.reconnect_window;
    while (true) {
      try {
        return session.connector->connect();
      } catch (const RemoteTransportError& error) {
        reason = std::string("cannot reach worker: ") + error.what();
        if (error.code() == RemoteErrorCode::cancelled ||
            error.code() == RemoteErrorCode::authentication_failed ||
            std::chrono::steady_clock::now() + options.reconnect_pause >= give_up) {
          return std::nullopt;
        }
      }
      std::unique_lock lock(mutex);
      if (session_finished.wait_for(lock, options.reconnect_pause,
                                    [&] { return session.retiring || stopping; })) {
        return std::nullopt;
      }
    }
  }

  void run_session(Session& session) {
    WorkerSessionEnd end{.failure = AttemptFailureKind::executor_lost,
                         .reason = "worker session closed while connecting"};
    if (options.before_connect) {
      try {
        options.before_connect([this, &session] {
          const std::lock_guard lock(mutex);
          return session.retiring || stopping;
        });
      } catch (const std::exception&) {
        // A wait that fails leaves connecting to decide.
      }
    }
    std::optional<RemoteConnection> connection = connect_within_window(session, end.reason);
    if (connection && adopt_connection(session, std::move(*connection))) {
      StreamFrameReader reader(*session.stream, options.frame_limits);
      bool ready = true;
      if (options.session_preamble) {
        try {
          options.session_preamble(reader, *session.writer, *session.stream);
        } catch (const std::exception& error) {
          ready = false;
          end.reason = std::string("worker session setup failed: ") + error.what();
        }
      }
      if (ready) {
        std::vector<PendingAssign> pending;
        mark_ready(session, pending);
        try {
          for (const PendingAssign& assign : pending) {
            session.writer->write(assign.frame);
          }
        } catch (const ExecError&) {
          // The connection is gone; the reader below sees it end.
        }
        end = pump_worker_frames(reader, mutex, session.leases, *events,
                                 "worker connection ended");
      }
      session.stream->cancel();
    }

    std::vector<std::string> orphaned;
    bool report = false;
    {
      const std::lock_guard lock(mutex);
      session.finished = true;
      session.pending.clear();
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

  // Caller holds the mutex.
  void end_session_now(Session& session) {
    if (session.connected) {
      session.stream->cancel();
    } else {
      session.connector->cancel();
      // Wakes a session pausing between reconnect attempts.
      session_finished.notify_all();
    }
  }

  void close_session(Session& session, std::unique_lock<std::mutex>& lock) {
    if (!session.finished) {
      if (session.connected) {
        lock.unlock();
        try {
          session.writer->write(make_shutdown_frame());
        } catch (const ExecError&) {
          // Already gone; the reader is finishing.
        }
        lock.lock();
        if (!session_finished.wait_for(lock, options.shutdown_grace,
                                       [&] { return session.finished; })) {
          end_session_now(session);
        }
      } else {
        end_session_now(session);
      }
    }
    lock.unlock();
    if (session.thread.joinable()) {
      session.thread.join();
    }
    lock.lock();
  }

  RemoteExecutorOptions options;
  ExecutorEvents* events = nullptr;
  mutable std::mutex mutex;
  std::condition_variable session_finished;
  std::shared_ptr<Session> current;
  std::vector<std::shared_ptr<Session>> retired;
  std::size_t connections_opened = 0;
  std::optional<RouteChoice> last_route;
  bool stopping = false;
};

RemoteExecutor::RemoteExecutor(RemoteExecutorOptions options)
    : state_(std::make_unique<State>(std::move(options))) {
  const RemoteExecutorOptions& checked = state_->options;
  if (checked.slots == 0 || checked.executor_id.empty() ||
      checked.reconnect_window.count() < 0 || checked.reconnect_pause.count() <= 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "remote executor needs an executor id, at least one slot, a non-negative "
                    "reconnect window, and a positive reconnect pause");
  }
  validate_pairing_key(checked.connector.pairing);
  validate_route_policy(checked.connector.routes);
  validate_transport_policy(checked.connector.transport);
}

RemoteExecutor::~RemoteExecutor() { stop(); }

std::string_view RemoteExecutor::id() const { return state_->options.executor_id; }

std::size_t RemoteExecutor::slots() const { return state_->options.slots; }

LossQuarantine RemoteExecutor::loss_quarantine() const {
  return LossQuarantine::after_repeated_losses;
}

std::size_t RemoteExecutor::connections_opened() const {
  const std::lock_guard lock(state_->mutex);
  return state_->connections_opened;
}

std::optional<RouteChoice> RemoteExecutor::last_route() const {
  const std::lock_guard lock(state_->mutex);
  return state_->last_route;
}

void RemoteExecutor::start(ExecutorEvents& events) { state_->events = &events; }

void RemoteExecutor::assign(const TaskSpec& spec, const Lease& lease) {
  State& state = *state_;
  Frame frame = make_leased_assign_frame(spec, lease);
  std::shared_ptr<Session> session;
  {
    const std::lock_guard lock(state.mutex);
    session = state.live_session();
    session->leases.add(lease.lease_id, spec.task_id, lease.attempt);
    if (!session->ready) {
      session->pending.push_back(PendingAssign{.lease_id = lease.lease_id, .frame = std::move(frame)});
      return;
    }
  }
  try {
    session->writer->write(frame);
  } catch (const ExecError&) {
    // The connection is gone; its reader reports this lease as lost.
  }
}

void RemoteExecutor::cancel(std::string_view lease_id) {
  State& state = *state_;
  std::shared_ptr<Session> session;
  {
    const std::lock_guard lock(state.mutex);
    session = state.session_for(lease_id);
    if (!session || session->finished) {
      return;
    }
    session->leases.mark_cancelled(lease_id);
    if (!session->ready) {
      std::erase_if(session->pending,
                    [&](const PendingAssign& pending) { return pending.lease_id == lease_id; });
      return;
    }
  }
  try {
    session->writer->write(make_cancel_frame(lease_id));
  } catch (const ExecError&) {
    // The connection is gone; nothing to cancel.
  }
}

void RemoteExecutor::lease_expired(std::string_view lease_id) {
  State& state = *state_;
  const std::lock_guard lock(state.mutex);
  const std::shared_ptr<Session> session = state.session_for(lease_id);
  if (!session || session->finished) {
    return;
  }
  session->leases.mark_cancelled(lease_id);
  // The session thread sees the connection end and fails the rest.
  session->retiring = true;
  state.end_session_now(*session);
}

bool RemoteExecutor::close_idle_session() {
  State& state = *state_;
  std::shared_ptr<Session> session;
  {
    const std::lock_guard lock(state.mutex);
    session = state.current;
    if (!session || session->finished || session->retiring || state.stopping ||
        !session->leases.empty() || !session->pending.empty()) {
      return false;
    }
    // New leases go to a new session from here on.
    session->retiring = true;
    if (!session->ready) {
      state.end_session_now(*session);
      return true;
    }
  }
  try {
    session->writer->write(make_shutdown_frame());
  } catch (const ExecError&) {
    // Already gone; the reader is finishing.
  }
  return true;
}

void RemoteExecutor::stop() {
  State& state = *state_;
  std::unique_lock lock(state.mutex);
  state.stopping = true;
  for (const std::shared_ptr<Session>& session : state.sessions()) {
    state.close_session(*session, lock);
  }
  state.retired.clear();
  state.current.reset();
}

}  // namespace svp::exec::remote
