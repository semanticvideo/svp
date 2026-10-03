#pragma once

// The worker agent's side of one coordinator session (plan §3.1, §4.3,
// §4.4). Transport-independent: it runs over any FrameReader / FrameWriter
// pair, so the same code serves an authenticated TLS connection in the
// launchd job and a socket pair in tests.
//
//   1. HELLO -> HELLO_ACK. The session is refused (and ends) when the
//      protocol major, CPU architecture, or macOS product version differs
//      from this worker's, or the coordinator's thread plan is not
//      host-independent (hello_messages.hpp evaluate_hello).
//   2. RUNTIME_HAVE / RUNTIME_PUT and BLOB_HAVE / BLOB_PUT: blobs into the
//      worker CAS, runtimes and model bundles assembled from them and
//      verified before they are installed (runtime_store.hpp,
//      model_bundles.hpp).
//   3. The first ASSIGN starts the session process (session_process.hpp)
//      from HELLO's runtime_id, after verifying every file of that runtime
//      against its manifest. ASSIGN passes memory admission (admission.hpp)
//      or is answered with REJECT; CANCEL and SHUTDOWN are forwarded; the
//      session process's HEARTBEAT / RESULT / ERROR frames are relayed back.
//   4. On SHUTDOWN, end of input, or a protocol error, the session process
//      gets the grace period to exit and is then killed, and the session's
//      scratch directory (and any partial blob) is deleted.
//   5. Every blob a BLOB_HAVE query names is pinned in the worker CAS for
//      the session's lifetime (holder `<session id>.agent`), so no other
//      coordinator's release deletes what this session still declares.
//      BLOB_RELEASE (protocol 1.2) hands blobs to the agent's ReleasedBlobs
//      (released_blobs.hpp), which deletes them once nothing pins them; the
//      agent tries again when each session ends, after its session process
//      has exited and its pins are gone.
//
// A put or query the worker cannot honour is answered with ERROR
// {"code","message"} and ends the session, as plan §4.3 defines ERROR.

#include "svp/exec/cas_store.hpp"
#include "svp/exec/frame_limits.hpp"
#include "svp/exec/frame_stream.hpp"
#include "svp/exec/loopback_executor.hpp"
#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/model_bundles.hpp"
#include "svp/exec/worker/released_blobs.hpp"
#include "svp/exec/worker/runtime_store.hpp"
#include "svp/exec/worker/session_process.hpp"
#include "svp/exec/worker/slot_sharing.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace svp::exec::worker {

struct AgentCoreOptions {
  WorkerLayout layout;
  HostFacts host;
  AdmissionPolicy admission{};
  // The runtime the agent itself runs from, when known (reported only).
  std::optional<Blake3Digest> agent_runtime_id;
  std::chrono::milliseconds shutdown_grace = kDefaultWorkerShutdownGrace;
  FrameLimits frame_limits{};
  // Injectable for tests; default sample_memory().
  std::function<MemorySnapshot()> sample_memory;
  // Injectable for tests; default spawn_session_process().
  SessionLauncher launcher;
  // This Mac's own build's running tasks per type (local_load.hpp); empty
  // when nothing local is counted (tests). run_worker_agent reads the
  // LocalLoad directory.
  SlotSharing::LocalLoad local_load;
  std::chrono::milliseconds slot_contention_window = kDefaultSlotContentionWindow;
};

// State shared by every session of one agent. Thread-safe.
class AgentCore {
 public:
  // Creates the worker layout and opens its CAS. Throws WorkerError(io).
  explicit AgentCore(AgentCoreOptions options);

  [[nodiscard]] const AgentCoreOptions& options() const noexcept { return options_; }
  [[nodiscard]] const WorkerLayout& layout() const noexcept { return options_.layout; }
  [[nodiscard]] AdmissionLedger& ledger() noexcept { return ledger_; }
  [[nodiscard]] SlotSharing& slots() noexcept { return slots_; }
  [[nodiscard]] ReleasedBlobs& released() noexcept { return released_; }
  [[nodiscard]] const WorkerRuntimeStore& runtimes() const noexcept { return runtimes_; }
  [[nodiscard]] const WorkerModelStore& models() const noexcept { return models_; }
  [[nodiscard]] CasStore cas() const { return cas_; }
  [[nodiscard]] MemorySnapshot memory() const;
  [[nodiscard]] SessionProcess launch(const SessionLaunch& launch) const;

  // HELLO_ACK describing this worker now.
  [[nodiscard]] WorkerHelloAck describe(const Blake3Digest& requested_runtime,
                                        std::optional<SessionRefusal> refusal) const;

  std::atomic<std::uint64_t> active_sessions{0};

 private:
  AgentCoreOptions options_;
  AdmissionLedger ledger_;
  SlotSharing slots_;
  ReleasedBlobs released_;
  WorkerRuntimeStore runtimes_;
  WorkerModelStore models_;
  CasStore cas_;
};

enum class AgentSessionEnd {
  refused,
  input_closed,
  shutdown,
  protocol_error,
  // The session process exited or sent bytes the agent could not relay.
  session_process_lost,
};

[[nodiscard]] std::string_view agent_session_end_name(AgentSessionEnd end) noexcept;

// Serves one session until it ends. `worker_session_id` must be a record
// identifier unique on this worker (it names the scratch directory and is
// stamped into results). `close_input` must make a blocked input.read()
// return (the transport's cancel); it is called when the session process
// is lost so the coordinator sees the session end. `coordinator_id` names
// who the session serves (the pairing it came in on) for slot sharing; empty
// counts the session as a coordinator of its own.
AgentSessionEnd serve_agent_session(AgentCore& core, FrameReader& input, FrameWriter& output,
                                    const std::string& worker_session_id,
                                    const std::function<void()>& close_input,
                                    std::string_view coordinator_id = {});

}  // namespace svp::exec::worker
