#include "svp/exec/journal_error.hpp"

#include <utility>

namespace svp::exec {

std::string_view journal_error_code_name(JournalErrorCode code) noexcept {
  switch (code) {
    case JournalErrorCode::already_exists:
      return "already_exists";
    case JournalErrorCode::not_found:
      return "not_found";
    case JournalErrorCode::locked:
      return "locked";
    case JournalErrorCode::incompatible:
      return "incompatible";
    case JournalErrorCode::io_error:
      return "io_error";
    case JournalErrorCode::database_error:
      return "database_error";
    case JournalErrorCode::duplicate_record:
      return "duplicate_record";
    case JournalErrorCode::source_mismatch:
      return "source_mismatch";
    case JournalErrorCode::unknown_task:
      return "unknown_task";
    case JournalErrorCode::invalid_transition:
      return "invalid_transition";
    case JournalErrorCode::invalid_argument:
      return "invalid_argument";
    case JournalErrorCode::artifact_mismatch:
      return "artifact_mismatch";
    case JournalErrorCode::closed:
      return "closed";
  }
  return "unknown";
}

JournalError::JournalError(JournalErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

JournalErrorCode JournalError::code() const noexcept {
  return code_;
}

}  // namespace svp::exec
