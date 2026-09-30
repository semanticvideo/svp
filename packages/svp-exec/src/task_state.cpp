#include "svp/exec/task_state.hpp"

#include <array>
#include <utility>

namespace svp::exec {
namespace {

constexpr std::array<std::pair<TaskState, std::string_view>, 8> kTaskStateNames = {{
    {TaskState::planned, "planned"},
    {TaskState::ready, "ready"},
    {TaskState::leased, "leased"},
    {TaskState::running, "running"},
    {TaskState::result_received, "result_received"},
    {TaskState::committed, "committed"},
    {TaskState::failed_retryable, "failed_retryable"},
    {TaskState::failed_permanent, "failed_permanent"},
}};

// Plan §4.4 lifecycle edges. Local execution may skip `leased`.
constexpr std::array<std::pair<TaskState, TaskState>, 16> kAllowedTransitions = {{
    {TaskState::planned, TaskState::ready},
    {TaskState::ready, TaskState::leased},
    {TaskState::ready, TaskState::running},
    {TaskState::ready, TaskState::result_received},  // cache hit
    {TaskState::leased, TaskState::running},
    {TaskState::leased, TaskState::ready},  // lease expired before start
    {TaskState::leased, TaskState::failed_retryable},
    {TaskState::running, TaskState::result_received},
    {TaskState::running, TaskState::ready},  // lease expired or worker lost
    {TaskState::running, TaskState::failed_retryable},
    {TaskState::running, TaskState::failed_permanent},
    {TaskState::result_received, TaskState::committed},
    {TaskState::result_received, TaskState::failed_retryable},  // result invalid
    {TaskState::result_received, TaskState::failed_permanent},
    {TaskState::failed_retryable, TaskState::ready},
    {TaskState::failed_retryable, TaskState::failed_permanent},  // retries exhausted
}};

}  // namespace

std::string_view task_state_name(TaskState state) noexcept {
  for (const auto& [value, name] : kTaskStateNames) {
    if (value == state) {
      return name;
    }
  }
  return "unknown";
}

std::optional<TaskState> parse_task_state(std::string_view name) noexcept {
  for (const auto& [value, known] : kTaskStateNames) {
    if (known == name) {
      return value;
    }
  }
  return std::nullopt;
}

bool is_task_transition_allowed(TaskState from, TaskState to) noexcept {
  for (const auto& [allowed_from, allowed_to] : kAllowedTransitions) {
    if (allowed_from == from && allowed_to == to) {
      return true;
    }
  }
  return false;
}

}  // namespace svp::exec
