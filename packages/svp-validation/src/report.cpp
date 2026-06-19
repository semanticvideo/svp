#include "svp/validation/report.hpp"

#include <algorithm>

namespace svp::validation {

const char* to_string(FindingSeverity severity) noexcept {
  switch (severity) {
    case FindingSeverity::info:
      return "info";
    case FindingSeverity::warning:
      return "warning";
    case FindingSeverity::error:
      return "error";
  }

  return "error";
}

bool passed(const ValidationReport& report) noexcept {
  return std::none_of(report.findings.begin(), report.findings.end(),
                      [](const ValidationFinding& finding) {
                        return finding.severity == FindingSeverity::error;
                      });
}

}  // namespace svp::validation

