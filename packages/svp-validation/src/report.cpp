#include "svp/validation/report.hpp"

#include <algorithm>
#include <utility>

namespace svp::validation {

const char* to_string(FindingSeverity severity) noexcept {
  switch (severity) {
    case FindingSeverity::info:
      return "info";
    case FindingSeverity::warning:
      return "warning";
    case FindingSeverity::error:
      return "error";
    case FindingSeverity::fatal:
      return "fatal";
  }

  return "error";
}

const char* to_string(ValidationStatus status) noexcept {
  switch (status) {
    case ValidationStatus::valid:
      return "valid";
    case ValidationStatus::valid_with_warnings:
      return "valid_with_warnings";
    case ValidationStatus::invalid:
      return "invalid";
    case ValidationStatus::unreadable:
      return "unreadable";
  }

  return "invalid";
}

const char* to_string(AuthenticityStatus status) noexcept {
  switch (status) {
    case AuthenticityStatus::not_checked:
      return "not_checked";
  }

  return "not_checked";
}

int exit_code(const ValidationReport& report) noexcept {
  if (report.status == ValidationStatus::unreadable) {
    return 2;
  }

  if (report.core_status == ValidationStatus::invalid) {
    return 1;
  }

  return 0;
}

void add_finding(ValidationReport& report, ValidationFinding finding) {
  switch (finding.severity) {
    case FindingSeverity::info:
      report.infos.push_back(std::move(finding));
      break;
    case FindingSeverity::warning:
      report.warnings.push_back(std::move(finding));
      break;
    case FindingSeverity::error:
    case FindingSeverity::fatal:
      report.errors.push_back(std::move(finding));
      break;
  }
}

void recompute_status(ValidationReport& report) noexcept {
  const auto affects_core = [](const ValidationFinding& finding) {
    return finding.affects_core_status;
  };

  const bool has_core_errors =
      std::any_of(report.errors.begin(), report.errors.end(), affects_core);
  if (has_core_errors) {
    report.core_status = ValidationStatus::invalid;
    report.status = ValidationStatus::invalid;
    return;
  }

  const bool has_core_warnings =
      std::any_of(report.warnings.begin(), report.warnings.end(), affects_core);
  if (has_core_warnings) {
    report.core_status = ValidationStatus::valid_with_warnings;
    report.status = ValidationStatus::valid_with_warnings;
    return;
  }

  report.core_status = ValidationStatus::valid;
  report.status = ValidationStatus::valid;
}

}  // namespace svp::validation
