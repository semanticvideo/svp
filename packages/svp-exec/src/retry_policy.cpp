#include "svp/exec/retry_policy.hpp"

#include "svp/exec/exec_error.hpp"

namespace svp::exec {

void validate_retry_policy(const RetryPolicy& policy) {
  if (policy.max_attempts == 0 || policy.quarantine_after_loss_events == 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "retry policy needs max_attempts >= 1 and "
                    "quarantine_after_loss_events >= 1");
  }
}

}  // namespace svp::exec
