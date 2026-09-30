#include "svp/exec/exec_error.hpp"

#include <utility>

namespace svp::exec {

std::string_view exec_error_code_name(ExecErrorCode code) noexcept {
  switch (code) {
    case ExecErrorCode::invalid_json:
      return "invalid_json";
    case ExecErrorCode::non_canonical_json:
      return "non_canonical_json";
    case ExecErrorCode::non_finite_number:
      return "non_finite_number";
    case ExecErrorCode::missing_field:
      return "missing_field";
    case ExecErrorCode::unknown_field:
      return "unknown_field";
    case ExecErrorCode::wrong_type:
      return "wrong_type";
    case ExecErrorCode::invalid_value:
      return "invalid_value";
    case ExecErrorCode::invalid_digest:
      return "invalid_digest";
    case ExecErrorCode::digest_mismatch:
      return "digest_mismatch";
    case ExecErrorCode::unknown_message_type:
      return "unknown_message_type";
    case ExecErrorCode::frame_header_too_large:
      return "frame_header_too_large";
    case ExecErrorCode::frame_payload_too_large:
      return "frame_payload_too_large";
    case ExecErrorCode::frame_truncated:
      return "frame_truncated";
    case ExecErrorCode::frame_malformed:
      return "frame_malformed";
    case ExecErrorCode::payload_hash_mismatch:
      return "payload_hash_mismatch";
    case ExecErrorCode::unknown_task_type:
      return "unknown_task_type";
    case ExecErrorCode::duplicate_task_type:
      return "duplicate_task_type";
    case ExecErrorCode::invalid_task_parameters:
      return "invalid_task_parameters";
    case ExecErrorCode::unresolved_input:
      return "unresolved_input";
  }
  return "unknown";
}

ExecError::ExecError(ExecErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ExecErrorCode ExecError::code() const noexcept {
  return code_;
}

}  // namespace svp::exec
