#pragma once

#include <optional>
#include <string_view>

namespace svp::exec {

// Task lifecycle (plan §4.4), stored in `task.status`:
//
//   planned -> ready -> leased -> running -> result_received -> committed
//                 ^        |          |              |
//                 |        v          v              v
//                 +--- failed_retryable        failed_permanent
//
// Lease expiry or a lost worker returns leased/running to ready. A cache hit
// goes ready -> result_received directly. `committed` is reached only through
// RecoveryJournal::commit_task, which records the artifacts in the same
// transaction.
enum class TaskState {
  planned,
  ready,
  leased,
  running,
  result_received,
  committed,
  failed_retryable,
  failed_permanent,
};

[[nodiscard]] std::string_view task_state_name(TaskState state) noexcept;
[[nodiscard]] std::optional<TaskState> parse_task_state(std::string_view name) noexcept;

// The edges drawn above. Resume re-derives states outside this table.
[[nodiscard]] bool is_task_transition_allowed(TaskState from, TaskState to) noexcept;

}  // namespace svp::exec
