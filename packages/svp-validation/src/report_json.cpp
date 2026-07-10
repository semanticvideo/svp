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
  if (report.embedding_transport.present) {
    const auto& transport = report.embedding_transport;
    json["embedding_transport"] = {
        {"embedding_detected", transport.embedding_detected},
        {"container_supported", transport.container_supported},
        {"container_kind", transport.container_kind},
        {"major_brand", transport.major_brand},
        {"compatible_brands", transport.compatible_brands},
        {"profile", transport.profile},
        {"uuid", transport.uuid},
        {"status", transport.status},
        {"embedded_package_status", transport.embedded_package_status},
        {"profile_version", transport.profile_version},
        {"box_offset", transport.box_offset},
        {"box_size", transport.box_size},
        {"payload_offset", transport.payload_offset},
        {"payload_size", transport.payload_size},
        {"payload_hash_status", transport.payload_hash_status},
    };
  }
}

}  // namespace svp::validation
