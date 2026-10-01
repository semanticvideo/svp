#include "svp/exec/scheduler_policy.hpp"

#include "svp/exec/exec_error.hpp"

namespace svp::exec {

void validate_scheduler_policy(const SchedulerPolicy& policy) {
  validate_lease_policy(policy.lease);
  validate_retry_policy(policy.retry);
  if (policy.max_idle_wait.count() <= 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "scheduler policy max_idle_wait must be positive");
  }
  if (policy.rejection_backoff.count() <= 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "scheduler policy rejection_backoff must be positive");
  }
}

}  // namespace svp::exec
