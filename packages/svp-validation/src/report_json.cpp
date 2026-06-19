#include "svp/validation/report_json.hpp"

namespace svp::validation {

void to_json(nlohmann::json& json, const ValidationFinding& finding) {
  json = nlohmann::json{
      {"code", finding.code},
      {"severity", to_string(finding.severity)},
      {"message", finding.message},
      {"path", finding.path},
  };
}

void to_json(nlohmann::json& json, const ValidatorIdentity& validator) {
  json = nlohmann::json{
      {"name", validator.name},
      {"version", validator.version},
  };
}

void to_json(nlohmann::json& json, const ValidationReport& report) {
  json = nlohmann::json{
      {"schema_version", report.schema_version},
      {"validator", report.validator},
      {"status", to_string(report.status)},
      {"core_status", to_string(report.core_status)},
      {"authenticity_status", to_string(report.authenticity_status)},
      {"errors", report.errors},
      {"warnings", report.warnings},
      {"infos", report.infos},
      {"authenticity", report.authenticity},
  };
}

}  // namespace svp::validation
