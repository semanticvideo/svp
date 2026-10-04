#pragma once

// The worker agent (plan §3.3): what the launchd job runs. It serves every
// pairing record under <root>/pairings on ONE TLS-PSK listener with ONE
// Bonjour instance of its own plus one small instance per pairing for older
// coordinators (pairings_server.hpp), and serves each authenticated
// connection with serve_agent_session, the session's pairing proven in HELLO
// (pairing_proof.hpp). A pairing written or removed while it runs (fleet
// join, `workers pair`, `workers unpair`) is served at once (the pairings
// directory is watched). With a fleet join credential (<root>/join.json,
// `worker install --join`) it also runs the join listener (fleet_join.hpp)
// with its own instance. Advertising is retried in the background and never
// stops it (service_advertiser.hpp). It only accepts connections and never
// dials out, so macOS Local Network privacy does not apply to it in either
// launchd mode (Apple TN3179).
//
// It moves itself to a newer runtime when one arrives (service_updater.hpp)
// and then exits with kWorkerRestartExitCode for launchd to restart it.

#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/agent_session.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>

namespace svp::exec::worker {

// The agent's main thread only waits for signals (sigwait). Other threads
// wake it with this signal to restart after a self-update or to start the
// listeners a fleet join added; sent from outside it changes nothing.
inline constexpr int kAgentWakeSignal = SIGUSR1;

struct WorkerAgentOptions {
  std::filesystem::path root;
  AdmissionPolicy admission{};
  std::chrono::milliseconds shutdown_grace = kDefaultWorkerShutdownGrace;
  std::optional<Blake3Digest> agent_runtime_id;
  // Started through <root>/current (service_link.hpp); without it the agent
  // never moves itself to another runtime.
  bool launched_through_current = false;
};

// Serves until SIGINT or SIGTERM (returns 0) or a self-update (returns
// kWorkerRestartExitCode). Logs one line per event to stderr (the job's
// log). Returns 0 at once when the root holds neither a pairing nor a join
// credential (launchd's KeepAlive {SuccessfulExit: false} then leaves the
// job stopped). Throws WorkerError when the root, a pairing record, or the
// join credential is unusable.
int run_worker_agent(const WorkerAgentOptions& options);

}  // namespace svp::exec::worker
