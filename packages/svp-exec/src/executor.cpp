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

}  // namespace svp::exec
