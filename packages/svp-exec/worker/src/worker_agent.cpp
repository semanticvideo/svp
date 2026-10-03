#include "svp/exec/worker/worker_agent.hpp"

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/remote_listener.hpp"
#include "svp/exec/worker/fleet_join.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/join_service.hpp"
#include "svp/exec/worker/local_load.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/pairings_server.hpp"
#include "svp/exec/worker/worker_identity.hpp"
#include "svp/exec/remote/service_advertiser.hpp"
#include "svp/exec/remote/stream_deadline.hpp"
#include "svp/exec/remote/transport_policy.hpp"
#include "svp/exec/worker/service_updater.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <set>
#include <unistd.h>
#include <vector>

namespace svp::exec::worker {
namespace {

std::mutex log_mutex;

void log_line(const std::string& line) {
  const std::lock_guard lock(log_mutex);
  std::cerr << utc_timestamp_now() << " svp-worker-agent: " << line << std::endl;
}

void wake_main_thread() { ::kill(::getpid(), kAgentWakeSignal); }

// What one agent serves. Changed on the main thread only (start, rescan);
// join handlers and the pairings directory watcher ask for a rescan and
// wake it.
class AgentListeners {
 public:
  AgentListeners(AgentCore& core, ServiceUpdater& updater, const WorkerLayout& layout,
                 std::string worker_id)
      : core_(core),
        updater_(updater),
        layout_(layout),
        worker_id_(worker_id),
        pairings_(PairingsServerOptions{
            .worker_id = std::move(worker_id),
            .serve =
                [this](remote::RemoteStream& stream, const remote::RemoteSessionInfo& info,
                       const CoordinatorResolver& resolve) { serve_session(stream, info, resolve); },
            .log = log_line,
            .advertise_each_pairing = true,
            .service_name = {},
            .transport = {}}) {}

  ~AgentListeners() { stop_all(); }

  // Serves exactly the pairings under <root>/pairings: one listener and one
  // advertisement for all of them (pairings_server.hpp).
  void rescan_pairings() {
    std::vector<remote::PairingKey> keys;
    for (const WorkerPairingRecord& record :
         load_worker_pairings(PairingDirectory(layout_.pairings()))) {
      keys.push_back(record.key);
    }
    const std::size_t before = pairings_.pairing_count();
    pairings_.set_pairings(keys);
    if (pairings_.pairing_count() != before || keys.size() != before) {
      log_line("serving " + std::to_string(keys.size()) + " pairing(s) on port " +
               std::to_string(pairings_.port()));
    }
  }

  // (Re)starts the join listener from <root>/join.json; none without one.
  void start_join() {
    join_advertisement_.reset();
    if (join_) {
      join_->stop();
      join_.reset();
    }
    // A credential that predates worker keys is migrated here first
    // (a new key-bound join id; join_service.hpp).
    const std::optional<WorkerJoinCredential> credential =
        load_current_join_credential(layout_, credential_mutex_);
    if (!credential) {
      return;
    }
    remote::RemoteListenerOptions listener_options;
    listener_options.pairing = join_listener_key(*credential);
    listener_options.advertise = false;
    join_ = std::make_unique<remote::RemoteListener>(
        listener_options, [this](remote::RemoteStream& stream,
                                 const remote::RemoteSessionInfo& info) { serve_join(stream, info); });
    join_->start();
    // Its own instance, named after its join id, so it never collides with
    // the worker's instance or another worker's.
    join_advertisement_ = std::make_unique<remote::ServiceAdvertiser>(
        remote::ServiceAdvertisement{
            .name = credential->worker_join_id,
            .port = join_->port(),
            .txt = {{std::string(remote::kPairingTxtKey), credential->worker_join_id},
                    {std::string(kFleetTxtKey), credential->token.fleet_id},
                    {std::string(kJoinTxtKey), join_txt_value(*credential)},
                    {std::string(remote::kWorkerTxtKey), worker_id_}}},
        log_line);
    log_line("joinable fleet=" + credential->token.fleet_id + " join=" +
             join_txt_value(*credential) + " id=" + credential->worker_join_id +
             " port=" + std::to_string(join_->port()));
  }

  // Main thread: applies what was asked for since the last call.
  void apply_requests() {
    bool restart_join = false;
    {
      const std::lock_guard lock(requests_mutex_);
      restart_join = join_restart_requested_;
      join_restart_requested_ = false;
    }
    try {
      rescan_pairings();
    } catch (const std::exception& error) {
      log_line("cannot serve the pairings under " + layout_.pairings().string() + ": " +
               error.what());
    }
    if (restart_join) {
      try {
        start_join();
      } catch (const std::exception& error) {
        log_line("cannot restart the join listener: " + std::string(error.what()));
      }
    }
  }

  void stop_all() {
    join_advertisement_.reset();
    if (join_) {
      join_->stop();
    }
    pairings_.stop();
  }

 private:
  void serve_session(remote::RemoteStream& stream, const remote::RemoteSessionInfo& info,
                     const CoordinatorResolver& resolve) {
    const std::string session_id =
        "ws-" + std::to_string(::getpid()) + "-" + std::to_string(info.session_number);
    if (!updater_.try_enter_session()) {
      log_line("session " + session_id + " from " + info.peer +
               " not served: the agent is switching to a newer runtime");
      return;
    }
    log_line("session " + session_id + " from " + info.peer + " started");
    StreamFrameReader reader(stream, core_.options().frame_limits);
    StreamFrameWriter writer(stream, core_.options().frame_limits);
    AgentSessionEnd end = AgentSessionEnd::protocol_error;
    std::string coordinator = "(unproven: an older coordinator)";
    const CoordinatorResolver logging_resolve = [&](const CoordinatorHello& hello) {
      const std::string id = resolve(hello);
      if (!id.empty()) {
        coordinator = "pairing=" + id;
      }
      return id;
    };
    try {
      end = serve_agent_session(core_, reader, writer, session_id, [&stream] { stream.cancel(); },
                                logging_resolve);
    } catch (const std::exception& error) {
      log_line("session " + session_id + " failed: " + error.what());
    }
    log_line("session " + session_id + " " + coordinator + " ended (" +
             std::string(agent_session_end_name(end)) + ")");
    updater_.leave_session();
  }

  void serve_join(remote::RemoteStream& stream, const remote::RemoteSessionInfo& info) {
    // A peer that stalls is cut off (its reads then end) instead of holding
    // this connection's thread.
    const remote::StreamDeadline deadline(stream, kJoinExchangeTimeout);
    StreamFrameReader reader(stream, core_.options().frame_limits);
    StreamFrameWriter writer(stream, core_.options().frame_limits);
    try {
      const JoinServiceOutcome outcome =
          handle_join_connection(reader, writer, layout_, credential_mutex_);
      log_line("fleet join from " + info.peer + " paired " + outcome.pairing_id);
      {
        const std::lock_guard lock(requests_mutex_);
        join_restart_requested_ = join_restart_requested_ || outcome.listener_key_changed;
      }
      wake_main_thread();
    } catch (const std::exception& error) {
      log_line("fleet join from " + info.peer + " refused: " + error.what());
    }
  }

  AgentCore& core_;
  ServiceUpdater& updater_;
  WorkerLayout layout_;
  std::string worker_id_;
  PairingsServer pairings_;
  std::unique_ptr<remote::RemoteListener> join_;
  std::unique_ptr<remote::ServiceAdvertiser> join_advertisement_;
  std::mutex credential_mutex_;
  std::mutex requests_mutex_;
  bool join_restart_requested_ = false;
};

// Wakes the main thread whenever <root>/pairings changes (a record written
// by a fleet join, `workers pair`, or removed by `workers unpair`), so the
// change is served at once without a restart.
class PairingsWatch {
 public:
  explicit PairingsWatch(const std::filesystem::path& directory)
      : queue_(dispatch_queue_create("org.svp.worker.pairings-watch", DISPATCH_QUEUE_SERIAL)) {
    fd_ = ::open(directory.c_str(), O_EVTONLY);
    if (fd_ < 0) {
      log_line("cannot watch " + directory.string() + "; pairing changes need a restart");
      return;
    }
    source_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, static_cast<uintptr_t>(fd_),
                                     DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND |
                                         DISPATCH_VNODE_ATTRIB | DISPATCH_VNODE_LINK,
                                     queue_);
    dispatch_source_set_event_handler(source_, ^{
      wake_main_thread();
    });
    const int fd = fd_;
    dispatch_source_set_cancel_handler(source_, ^{
      ::close(fd);
    });
    dispatch_resume(source_);
  }
  ~PairingsWatch() {
    if (source_ != nullptr) {
      dispatch_source_cancel(source_);
      dispatch_release(source_);
    } else if (fd_ >= 0) {
      ::close(fd_);
    }
    dispatch_release(queue_);
  }
  PairingsWatch(const PairingsWatch&) = delete;
  PairingsWatch& operator=(const PairingsWatch&) = delete;

 private:
  dispatch_queue_t queue_;
  dispatch_source_t source_ = nullptr;
  int fd_ = -1;
};

}  // namespace

int run_worker_agent(const WorkerAgentOptions& options) {
  // Signals are taken synchronously by sigwait below; block them before any
  // thread (listener, session) starts so none of those threads takes them.
  sigset_t wait_signals;
  sigemptyset(&wait_signals);
  sigaddset(&wait_signals, SIGINT);
  sigaddset(&wait_signals, SIGTERM);
  sigaddset(&wait_signals, kAgentWakeSignal);
  pthread_sigmask(SIG_BLOCK, &wait_signals, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  const WorkerLayout layout{.root = options.root};
  create_worker_layout(layout);
  clear_worker_scratch(layout);
  const std::vector<WorkerPairingRecord> pairings =
      load_worker_pairings(PairingDirectory(layout.pairings()));
  const bool joinable = load_worker_join_credential(layout).has_value();
  if (pairings.empty() && !joinable) {
    log_line("no pairings under " + layout.pairings().string() + " and no join credential; " +
             "exiting");
    return 0;
  }

  const std::string worker_id = load_or_create_worker_id(layout);
  ServiceUpdater updater(ServiceUpdaterOptions{
      .layout = layout,
      .own_runtime = options.agent_runtime_id,
      .launched_through_current = options.launched_through_current,
      .test_start = {},
      .log = log_line,
      .request_restart = wake_main_thread});
  AgentCore core(AgentCoreOptions{.layout = layout,
                                  .host = detect_host_facts(),
                                  .admission = options.admission,
                                  .agent_runtime_id = options.agent_runtime_id,
                                  .shutdown_grace = options.shutdown_grace,
                                  .frame_limits = {},
                                  .sample_memory = {},
                                  .launcher = {},
                                  // This Mac's own --distributed build, if
                                  // any (local_load.hpp).
                                  .local_load = [directory = default_local_load_dir()] {
                                    return read_local_load(directory);
                                  },
                                  .slot_contention_window = kDefaultSlotContentionWindow,
                                  .runtime_installed = [&updater] { updater.runtime_installed(); },
                                  .service_state = [&updater] { return updater.state(); },
                                  .worker_id = worker_id});
  const HostFacts& host = core.options().host;
  log_line("starting pid=" + std::to_string(::getpid()) + " macOS " + host.os.product_version +
           " (" + host.os.build + ") " + host.arch + " cpus=" + std::to_string(host.logical_cpus) +
           " memory=" + std::to_string(host.physical_memory_bytes) +
           " reserve=" + std::to_string(core.ledger().reserve_bytes()) + " worker=" + worker_id +
           (options.agent_runtime_id ? " runtime=" + blake3_prefixed(*options.agent_runtime_id)
                                     : std::string()));
  if (!updater.enabled()) {
    log_line("self-update off: " + updater.disabled_reason());
  } else if (updater.consider() == UpdateStep::switched) {
    clear_worker_scratch(layout);
    return kWorkerRestartExitCode;
  }

  AgentListeners listeners(core, updater, layout, worker_id);
  // Only a listener that cannot open ends the agent (launchd restarts it);
  // advertising never does (pairings_server.hpp).
  listeners.rescan_pairings();
  if (joinable) {
    listeners.start_join();
  }
  const PairingsWatch watch(layout.pairings());

  int exit_code = 0;
  while (true) {
    int signal_number = 0;
    sigwait(&wait_signals, &signal_number);
    if (signal_number != kAgentWakeSignal) {
      log_line("stopping on signal " + std::to_string(signal_number));
      break;
    }
    if (updater.restart_requested()) {
      log_line("stopping to restart on the new runtime");
      exit_code = kWorkerRestartExitCode;
      break;
    }
    listeners.apply_requests();
  }
  listeners.stop_all();
  clear_worker_scratch(layout);
  return exit_code;
}

}  // namespace svp::exec::worker
