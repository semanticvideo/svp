#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace svp::exec {

// Every contract violation detected by svp-exec is reported with one of these
// codes so callers (scheduler, worker, tests) can react without parsing
// messages.
enum class ExecErrorCode {
  invalid_json,
  non_canonical_json,
  non_finite_number,
  missing_field,
  unknown_field,
  wrong_type,
  invalid_value,
  invalid_digest,
  digest_mismatch,
  unknown_message_type,
  frame_header_too_large,
  frame_payload_too_large,
  frame_truncated,
  frame_malformed,
  payload_hash_mismatch,
  unknown_task_type,
  duplicate_task_type,
  invalid_task_parameters,
  unresolved_input,
};

[[nodiscard]] std::string_view exec_error_code_name(ExecErrorCode code) noexcept;

class ExecError : public std::runtime_error {
 public:
  ExecError(ExecErrorCode code, std::string message);

  [[nodiscard]] ExecErrorCode code() const noexcept;

 private:
  ExecErrorCode code_;
};

}  // namespace svp::exec
