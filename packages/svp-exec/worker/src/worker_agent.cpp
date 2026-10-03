#include "svp/exec/worker/worker_agent.hpp"

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/remote_listener.hpp"
#include "svp/exec/worker/fleet_join.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/join_service.hpp"
#include "svp/exec/worker/local_load.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/service_updater.hpp"
#include "svp/exec/worker/worker_error.hpp"

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

// Session ids carry the tail of the pairing id so a log line shows which
// coordinator a session belongs to without printing the whole id.
constexpr std::size_t kSessionIdPairingChars = 6;

void log_line(const std::string& line) {
  const std::lock_guard lock(log_mutex);
  std::cerr << utc_timestamp_now() << " svp-worker-agent: " << line << std::endl;
}

std::string pairing_tail(const std::string& pairing_id) {
  return pairing_id.substr(pairing_id.size() > kSessionIdPairingChars
                               ? pairing_id.size() - kSessionIdPairingChars
                               : 0);
}

void wake_main_thread() { ::kill(::getpid(), kAgentWakeSignal); }

// Listeners of one agent. Started and stopped on the main thread only; join
// handlers ask for changes through request_*() and wake it.
class AgentListeners {
 public:
  AgentListeners(AgentCore& core, ServiceUpdater& updater, const WorkerLayout& layout)
      : core_(core), updater_(updater), layout_(layout) {}

  ~AgentListeners() { stop_all(); }

  void start_pairing(const WorkerPairingRecord& pairing) {
    const std::string pairing_id = pairing.key.pairing_id;
    if (const auto found = pairings_.find(pairing_id); found != pairings_.end()) {
      found->second->stop();
      pairings_.erase(found);
    }
    remote::RemoteListenerOptions listener_options;
    listener_options.pairing = pairing.key;
    auto listener = std::make_unique<remote::RemoteListener>(
        listener_options,
        [this, pairing_id](remote::RemoteStream& stream, const remote::RemoteSessionInfo& info) {
          serve_session(stream, info, pairing_id);
        });
    listener->start();
    log_line("listening pairing=" + pairing_id + " service=" + listener->advertised_name() +
             " port=" + std::to_string(listener->port()));
    pairings_.emplace(pairing_id, std::move(listener));
  }

  // (Re)starts the join listener from <root>/join.json; none without one.
  void start_join() {
    if (join_) {
      join_->stop();
      join_.reset();
    }
    std::optional<WorkerJoinCredential> credential;
    {
      const std::lock_guard lock(credential_mutex_);
      credential = load_worker_join_credential(layout_);
    }
    if (!credential) {
      return;
    }
    remote::RemoteListenerOptions listener_options;
    listener_options.pairing = join_listener_key(*credential);
    listener_options.txt = {{std::string(kFleetTxtKey), credential->token.fleet_id},
                           {std::string(kJoinTxtKey), join_txt_value(*credential)}};
    join_ = std::make_unique<remote::RemoteListener>(
        listener_options, [this](remote::RemoteStream& stream,
                                 const remote::RemoteSessionInfo& info) { serve_join(stream, info); });
    join_->start();
    log_line("joinable fleet=" + credential->token.fleet_id + " join=" +
             join_txt_value(*credential) + " id=" + credential->worker_join_id +
             " port=" + std::to_string(join_->port()));
  }

  // Main thread: applies what join handlers asked for since the last call.
  void apply_requests() {
    std::vector<std::string> pairing_ids;
    bool restart_join = false;
    {
      const std::lock_guard lock(requests_mutex_);
      pairing_ids.assign(requested_pairings_.begin(), requested_pairings_.end());
      requested_pairings_.clear();
      restart_join = join_restart_requested_;
      join_restart_requested_ = false;
    }
    for (const std::string& pairing_id : pairing_ids) {
      try {
        const std::optional<std::string> bytes =
            PairingDirectory(layout_.pairings()).read(pairing_id);
        if (bytes) {
          start_pairing(decode_worker_pairing(*bytes));
        }
      } catch (const std::exception& error) {
        log_line("cannot start the listener of pairing " + pairing_id + ": " + error.what());
      }
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
    if (join_) {
      join_->stop();
    }
    for (auto& [id, listener] : pairings_) {
      listener->stop();
    }
  }

 private:
  void serve_session(remote::RemoteStream& stream, const remote::RemoteSessionInfo& info,
                     const std::string& pairing_id) {
    const std::string session_id = "ws-" + std::to_string(::getpid()) + "-" +
                                   pairing_tail(pairing_id) + "-" +
                                   std::to_string(info.session_number);
    if (!updater_.try_enter_session()) {
      log_line("session " + session_id + " from " + info.peer +
               " not served: the agent is switching to a newer runtime");
      return;
    }
    log_line("session " + session_id + " from " + info.peer + " started");
    StreamFrameReader reader(stream, core_.options().frame_limits);
    StreamFrameWriter writer(stream, core_.options().frame_limits);
    AgentSessionEnd end = AgentSessionEnd::protocol_error;
    try {
      end = serve_agent_session(core_, reader, writer, session_id, [&stream] { stream.cancel(); },
                                pairing_id);
    } catch (const std::exception& error) {
      log_line("session " + session_id + " failed: " + error.what());
    }
    log_line("session " + session_id + " ended (" + std::string(agent_session_end_name(end)) +
             ")");
    updater_.leave_session();
  }

  void serve_join(remote::RemoteStream& stream, const remote::RemoteSessionInfo& info) {
    StreamFrameReader reader(stream, core_.options().frame_limits);
    StreamFrameWriter writer(stream, core_.options().frame_limits);
    try {
      const JoinServiceOutcome outcome =
          handle_join_connection(reader, writer, layout_, credential_mutex_);
      log_line("fleet join from " + info.peer + " paired " + outcome.pairing_id);
      {
        const std::lock_guard lock(requests_mutex_);
        requested_pairings_.insert(outcome.pairing_id);
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
  std::map<std::string, std::unique_ptr<remote::RemoteListener>> pairings_;
  std::unique_ptr<remote::RemoteListener> join_;
  std::mutex credential_mutex_;
  std::mutex requests_mutex_;
  std::set<std::string> requested_pairings_;
  bool join_restart_requested_ = false;
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
                                  .service_state = [&updater] { return updater.state(); }});
  const HostFacts& host = core.options().host;
  log_line("starting pid=" + std::to_string(::getpid()) + " macOS " + host.os.product_version +
           " (" + host.os.build + ") " + host.arch + " cpus=" + std::to_string(host.logical_cpus) +
           " memory=" + std::to_string(host.physical_memory_bytes) +
           " reserve=" + std::to_string(core.ledger().reserve_bytes()) +
           (options.agent_runtime_id ? " runtime=" + blake3_prefixed(*options.agent_runtime_id)
                                     : std::string()));
  if (!updater.enabled()) {
    log_line("self-update off: " + updater.disabled_reason());
  } else if (updater.consider() == UpdateStep::switched) {
    clear_worker_scratch(layout);
    return kWorkerRestartExitCode;
  }

  AgentListeners listeners(core, updater, layout);
  for (const WorkerPairingRecord& pairing : pairings) {
    listeners.start_pairing(pairing);
  }
  if (joinable) {
    listeners.start_join();
  }

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
