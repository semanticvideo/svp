#pragma once

#include <string>
#include <vector>

namespace svp::validation {

enum class FindingSeverity {
  info,
  warning,
  error,
};

struct ValidationFinding {
  std::string code;
  FindingSeverity severity = FindingSeverity::error;
  std::string message;
  std::string location;
};

struct ValidationReport {
  std::string validator_version;
  std::string package_path;
  std::vector<ValidationFinding> findings;
};

[[nodiscard]] const char* to_string(FindingSeverity severity) noexcept;
[[nodiscard]] bool passed(const ValidationReport& report) noexcept;

}  // namespace svp::validation

