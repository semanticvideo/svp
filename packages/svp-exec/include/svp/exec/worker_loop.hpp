#pragma once

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/frame_limits.hpp"
#include "svp/exec/task_artifact_access.hpp"
#include "svp/exec/task_registry.hpp"

#include <string>
#include <string_view>

namespace svp::exec {

struct WorkerLoopOptions {
  // Stamped into every TaskResult.execution (plan §4.2).
  std::string worker_session_id;
  Blake3Digest runtime_id{};
  FrameLimits frame_limits{};
};

enum class WorkerLoopExit {
  // The coordinator closed the stream.
  input_closed,
  // The coordinator sent SHUTDOWN.
  shutdown,
  // The coordinator sent something this loop cannot accept; an ERROR frame
  // was sent when the output was still writable (plan §4.3: ERROR ends the
  // session).
  protocol_error,
};

[[nodiscard]] std::string_view worker_loop_exit_name(WorkerLoopExit exit) noexcept;

// ERROR frame body the loop sends: {"code":"<lowercase id>","message":"..."}.
inline constexpr std::string_view kWorkerProtocolErrorCode = "protocol_error";

// Serves one coordinator session: reads ASSIGN / CANCEL / SHUTDOWN frames
// from `in_fd` and writes HEARTBEAT / RESULT / ERROR frames to `out_fd`.
// The same function serves the loopback test worker and, later, a remote
// worker session (plan §3.1 rule 1).
//
//   * ASSIGN starts the task on its own thread via run_task_attempt; the
//     coordinator bounds concurrency by the slots it advertised. A lease_id
//     that is already running is a protocol error.
//   * While any lease runs, one HEARTBEAT per lease is sent every
//     heartbeat_interval (the shortest interval among running leases).
//   * RESULT carries the TaskResult and its output payloads. A result whose
//     output bytes do not match its refs is replaced by a permanent failed
//     result (invalid_value), never sent as a success.
//   * CANCEL drops the lease: its result is not sent and its attempt's
//     CancellationToken is set, so the task function stops at its next
//     cooperative check. Task functions are not otherwise interrupted; the
//     process owner enforces a grace period (plan §4.4).
//   * SHUTDOWN or end of input: every lease is dropped and cancelled, the
//     loop waits for running task functions to return, then returns.
//
// Does not close either descriptor.
WorkerLoopExit run_worker_loop(int in_fd, int out_fd, const TaskTypeRegistry& registry,
                               TaskArtifactAccess& artifacts,
                               const WorkerLoopOptions& options);

}  // namespace svp::exec
