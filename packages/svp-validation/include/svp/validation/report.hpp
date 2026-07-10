#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace svp::validation {

enum class FindingSeverity {
  info,
  warning,
  error,
  fatal,
};

enum class ValidationStatus {
  valid,
  valid_with_warnings,
  invalid,
  unreadable,
};

enum class AuthenticityStatus {
  not_checked,
};

struct ValidationFinding {
  std::string code;
  FindingSeverity severity = FindingSeverity::error;
  std::string message;
  std::string path;
  bool affects_core_status = true;
};

struct ValidatorIdentity {
  std::string name;
  std::string version;
};

struct EmbeddingTransportSummary {
  bool present = false;
  std::string profile;
  std::string uuid;
  std::string status;
  std::string embedded_package_status = "not_checked";
  std::uint16_t profile_version = 0;
  std::uint64_t box_offset = 0;
  std::uint64_t box_size = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_size = 0;
  std::string payload_hash_status;
};

struct ValidationReport {
  std::string schema_version = "svp-validation-report-v1";
  ValidatorIdentity validator;
  std::string package_path;
  ValidationStatus status = ValidationStatus::valid;
  ValidationStatus core_status = ValidationStatus::valid;
  AuthenticityStatus authenticity_status = AuthenticityStatus::not_checked;
  EmbeddingTransportSummary embedding_transport;
  std::vector<ValidationFinding> errors;
  std::vector<ValidationFinding> warnings;
  std::vector<ValidationFinding> infos;
  std::vector<ValidationFinding> authenticity;
};

[[nodiscard]] const char* to_string(FindingSeverity severity) noexcept;
[[nodiscard]] const char* to_string(ValidationStatus status) noexcept;
[[nodiscard]] const char* to_string(AuthenticityStatus status) noexcept;
[[nodiscard]] int exit_code(const ValidationReport& report) noexcept;

void add_finding(ValidationReport& report, ValidationFinding finding);
void recompute_status(ValidationReport& report) noexcept;

}  // namespace svp::validation
