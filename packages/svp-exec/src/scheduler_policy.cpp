#include "svp/exec/scheduler_policy.hpp"

#include "svp/exec/exec_error.hpp"

namespace svp::exec {

void validate_scheduler_policy(const SchedulerPolicy& policy) {
  validate_lease_policy(policy.lease);
  validate_retry_policy(policy.retry);
  for (const auto& [task_type, override_policy] : policy.task_types) {
    validate_lease_policy(override_policy.lease);
    RetryPolicy retry = policy.retry;
    retry.max_attempts = override_policy.max_attempts;
    validate_retry_policy(retry);
    if (task_type.empty()) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "scheduler policy task-type overrides need a task type");
    }
  }
  if (policy.max_idle_wait.count() <= 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "scheduler policy max_idle_wait must be positive");
  }
  if (policy.rejection_backoff.count() <= 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "scheduler policy rejection_backoff must be positive");
  }
}

const LeasePolicy& lease_policy_for(const SchedulerPolicy& policy, std::string_view task_type) {
  const auto found = policy.task_types.find(task_type);
  return found == policy.task_types.end() ? policy.lease : found->second.lease;
}

std::uint64_t max_attempts_for(const SchedulerPolicy& policy, std::string_view task_type) {
  const auto found = policy.task_types.find(task_type);
  return found == policy.task_types.end() ? policy.retry.max_attempts
                                          : found->second.max_attempts;
}

}  // namespace svp::exec
