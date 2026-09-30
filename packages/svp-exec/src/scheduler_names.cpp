#include "svp/exec/attempt_event.hpp"
#include "svp/exec/build_outcome.hpp"

namespace svp::exec {

std::string_view build_status_name(BuildStatus status) noexcept {
  switch (status) {
    case BuildStatus::succeeded:
      return "succeeded";
    case BuildStatus::failed:
      return "failed";
    case BuildStatus::cancelled:
      return "cancelled";
  }
  return "unknown";
}

std::string_view build_failure_kind_name(BuildFailureKind kind) noexcept {
  switch (kind) {
    case BuildFailureKind::retries_exhausted:
      return "retries_exhausted";
    case BuildFailureKind::permanent_task_failure:
      return "permanent_task_failure";
    case BuildFailureKind::determinism_incident:
      return "determinism_incident";
    case BuildFailureKind::no_usable_executor:
      return "no_usable_executor";
    case BuildFailureKind::commit_failed:
      return "commit_failed";
  }
  return "unknown";
}

std::string_view attempt_event_kind_name(AttemptEventKind kind) noexcept {
  switch (kind) {
    case AttemptEventKind::leased:
      return "leased";
    case AttemptEventKind::committed:
      return "committed";
    case AttemptEventKind::duplicate_discarded:
      return "duplicate_discarded";
    case AttemptEventKind::determinism_incident:
      return "determinism_incident";
    case AttemptEventKind::failed:
      return "failed";
    case AttemptEventKind::expired:
      return "expired";
    case AttemptEventKind::deadline_exceeded:
      return "deadline_exceeded";
    case AttemptEventKind::executor_quarantined:
      return "executor_quarantined";
  }
  return "unknown";
}

}  // namespace svp::exec
