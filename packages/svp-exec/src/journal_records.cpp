#include "svp/exec/journal_records.hpp"

#include "svp/exec/journal_resume.hpp"

namespace svp::exec {

std::string_view build_session_status_name(BuildSessionStatus status) noexcept {
  switch (status) {
    case BuildSessionStatus::active:
      return "active";
    case BuildSessionStatus::succeeded:
      return "succeeded";
    case BuildSessionStatus::failed:
      return "failed";
    case BuildSessionStatus::interrupted:
      return "interrupted";
  }
  return "unknown";
}

std::string_view worker_session_status_name(WorkerSessionStatus status) noexcept {
  switch (status) {
    case WorkerSessionStatus::active:
      return "active";
    case WorkerSessionStatus::ended:
      return "ended";
    case WorkerSessionStatus::lost:
      return "lost";
    case WorkerSessionStatus::quarantined:
      return "quarantined";
  }
  return "unknown";
}

std::string_view attempt_outcome_name(AttemptOutcome outcome) noexcept {
  switch (outcome) {
    case AttemptOutcome::leased:
      return "leased";
    case AttemptOutcome::running:
      return "running";
    case AttemptOutcome::succeeded:
      return "succeeded";
    case AttemptOutcome::failed:
      return "failed";
    case AttemptOutcome::lost:
      return "lost";
    case AttemptOutcome::abandoned:
      return "abandoned";
    case AttemptOutcome::duplicate:
      return "duplicate";
  }
  return "unknown";
}

std::string_view demotion_reason_name(DemotionReason reason) noexcept {
  switch (reason) {
    case DemotionReason::artifact_missing:
      return "artifact_missing";
    case DemotionReason::artifact_not_regular_file:
      return "artifact_not_regular_file";
    case DemotionReason::artifact_size_mismatch:
      return "artifact_size_mismatch";
    case DemotionReason::artifact_hash_mismatch:
      return "artifact_hash_mismatch";
    case DemotionReason::artifact_record_invalid:
      return "artifact_record_invalid";
    case DemotionReason::output_digest_missing:
      return "output_digest_missing";
  }
  return "unknown";
}

}  // namespace svp::exec
