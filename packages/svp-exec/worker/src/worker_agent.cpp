#include "svp/exec/worker/worker_agent.hpp"

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/remote_listener.hpp"
#include "svp/exec/worker/local_load.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <pthread.h>
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

}  // namespace

int run_worker_agent(const WorkerAgentOptions& options) {
  // Signals are taken synchronously by sigwait below; block them before any
  // thread (listener, session) starts so none of those threads takes them.
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGINT);
  sigaddset(&stop_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  const WorkerLayout layout{.root = options.root};
  create_worker_layout(layout);
  clear_worker_scratch(layout);
  const std::vector<WorkerPairingRecord> pairings =
      load_worker_pairings(PairingDirectory(layout.pairings()));
  if (pairings.empty()) {
    log_line("no pairings under " + layout.pairings().string() + "; exiting");
    return 0;
  }

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
                                  .slot_contention_window = kDefaultSlotContentionWindow});
  const HostFacts& host = core.options().host;
  log_line("starting pid=" + std::to_string(::getpid()) + " macOS " + host.os.product_version +
           " (" + host.os.build + ") " + host.arch + " cpus=" + std::to_string(host.logical_cpus) +
           " memory=" + std::to_string(host.physical_memory_bytes) +
           " reserve=" + std::to_string(core.ledger().reserve_bytes()) +
           (options.agent_runtime_id ? " runtime=" + blake3_prefixed(*options.agent_runtime_id)
                                     : std::string()));

  std::vector<std::unique_ptr<remote::RemoteListener>> listeners;
  for (const WorkerPairingRecord& pairing : pairings) {
    const std::string pairing_id = pairing.key.pairing_id;
    remote::RemoteListenerOptions listener_options;
    listener_options.pairing = pairing.key;
    auto listener = std::make_unique<remote::RemoteListener>(
        listener_options,
        [&core, pairing_id](remote::RemoteStream& stream, const remote::RemoteSessionInfo& info) {
          const std::string session_id = "ws-" + std::to_string(::getpid()) + "-" +
                                         pairing_id.substr(pairing_id.size() > kSessionIdPairingChars
                                                               ? pairing_id.size() -
                                                                     kSessionIdPairingChars
                                                               : 0) + "-" +
                                         std::to_string(info.session_number);
          log_line("session " + session_id + " from " + info.peer + " started");
          StreamFrameReader reader(stream, core.options().frame_limits);
          StreamFrameWriter writer(stream, core.options().frame_limits);
          AgentSessionEnd end = AgentSessionEnd::protocol_error;
          try {
            end = serve_agent_session(core, reader, writer, session_id,
                                      [&stream] { stream.cancel(); }, pairing_id);
          } catch (const std::exception& error) {
            log_line("session " + session_id + " failed: " + error.what());
          }
          log_line("session " + session_id + " ended (" +
                   std::string(agent_session_end_name(end)) + ")");
        });
    listener->start();
    log_line("listening pairing=" + pairing_id + " service=" + listener->advertised_name() +
             " port=" + std::to_string(listener->port()));
    listeners.push_back(std::move(listener));
  }

  int signal_number = 0;
  sigwait(&stop_signals, &signal_number);
  log_line("stopping on signal " + std::to_string(signal_number));
  for (const auto& listener : listeners) {
    listener->stop();
  }
  clear_worker_scratch(layout);
  return 0;
}

}  // namespace svp::exec::worker
