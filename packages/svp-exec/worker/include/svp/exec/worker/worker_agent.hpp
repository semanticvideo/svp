#pragma once

// The worker agent (plan §3.3): what the launchd job runs. It loads every
// pairing record under <root>/pairings, opens one RemoteListener per
// pairing (TLS 1.2 PSK with that pairing's secret, Bonjour
// _svp-worker._tcp with its pairing id in TXT), and serves each
// authenticated connection with serve_agent_session. It only accepts
// connections and never dials out, so macOS Local Network privacy does not
// apply to it in either launchd mode (Apple TN3179).

#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/agent_session.hpp"

#include <chrono>
#include <filesystem>
#include <optional>

namespace svp::exec::worker {

struct WorkerAgentOptions {
  std::filesystem::path root;
  AdmissionPolicy admission{};
  std::chrono::milliseconds shutdown_grace = kDefaultWorkerShutdownGrace;
  std::optional<Blake3Digest> agent_runtime_id;
};

// Serves until SIGINT or SIGTERM. Logs one line per event to stderr (the
// job's log). Returns 0 after a clean stop, and 0 at once when the root
// holds no pairing (launchd's KeepAlive {SuccessfulExit: false} then leaves
// the job stopped). Throws WorkerError when the root or a pairing record is
// unusable.
int run_worker_agent(const WorkerAgentOptions& options);

}  // namespace svp::exec::worker
