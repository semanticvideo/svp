#include "svp/exec/executor.hpp"

namespace svp::exec {

std::string_view attempt_failure_kind_name(AttemptFailureKind kind) noexcept {
  switch (kind) {
    case AttemptFailureKind::invalid_result:
      return "invalid_result";
    case AttemptFailureKind::executor_lost:
      return "executor_lost";
  }
  return "unknown";
}

std::string_view loss_quarantine_name(LossQuarantine policy) noexcept {
  switch (policy) {
    case LossQuarantine::after_repeated_losses:
      return "after_repeated_losses";
    case LossQuarantine::never:
      return "never";
  }
  return "unknown";
}

}  // namespace svp::exec
