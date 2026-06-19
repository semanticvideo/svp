#include "svp/validation/report_json.hpp"

namespace svp::validation {

void to_json(nlohmann::json& json, const ValidationFinding& finding) {
  json = nlohmann::json{
      {"code", finding.code},
      {"severity", to_string(finding.severity)},
      {"message", finding.message},
      {"location", finding.location},
  };
}

void to_json(nlohmann::json& json, const ValidationReport& report) {
  json = nlohmann::json{
      {"validatorVersion", report.validator_version},
      {"packagePath", report.package_path},
      {"passed", passed(report)},
      {"findings", report.findings},
  };
}

}  // namespace svp::validation

