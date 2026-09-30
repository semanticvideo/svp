#pragma once

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/executor.hpp"
#include "svp/exec/task_artifact_access.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/exec/task_spec.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace svp::exec {

// Who is running an attempt; stamped into TaskResult.execution.
struct AttemptContext {
  std::uint64_t attempt = 0;
  std::string worker_session_id;
  Blake3Digest runtime_id{};
};

// Error codes of failed results produced by the runner itself (task-returned
// failures keep their own codes).
inline constexpr std::string_view kTaskExceptionErrorCode = "task_exception";

// The one path from a TaskSpec to an AttemptOutput (plan §3.1 rule 1: local
// and remote execution call the same function). Resolves inputs, runs the
// registered task function through TaskTypeRegistry::execute (admission,
// parameter validation, result validation), stamps attempt and execution
// identity, and reads the output bytes.
//
// Never throws for task-level problems; they become a failed TaskResult:
//   * ExecError: error.code is the ExecErrorCode name. Retryable only for
//     unresolved_input (bytes may arrive later); every other code is a
//     contract violation that repeats identically on every node running the
//     same runtime (plan §3.1 rule 2), so it is permanent.
//   * any other std::exception: kTaskExceptionErrorCode, retryable (resource
//     exhaustion and I/O errors can be transient).
[[nodiscard]] AttemptOutput run_task_attempt(const TaskTypeRegistry& registry,
                                             TaskArtifactAccess& artifacts,
                                             const TaskSpec& spec,
                                             const AttemptContext& context);

}  // namespace svp::exec
