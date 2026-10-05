#include "jsonl_record.hpp"

#include "export_error.hpp"
#include "reference_rules.hpp"

#include <string>

namespace package_export {

nlohmann::json parse_record(std::string_view record_text,
                            std::string_view entry, std::uint64_t line) {
  auto record = nlohmann::json::parse(record_text, nullptr, false);
  if (record.is_discarded()) {
    throw ExportError(ExportErrorCode::malformed_record,
                      "A JSONL record is not valid JSON.",
                      record_details(entry, line));
  }
  if (!record.is_object()) {
    throw ExportError(ExportErrorCode::malformed_record,
                      "A JSONL record is not a JSON object.",
                      record_details(entry, line));
  }
  if (record.contains(std::string{kExportMember})) {
    throw ExportError(ExportErrorCode::reserved_member_present,
                      "A package record already has the member the export "
                      "reserves for its additions (svp_export).",
                      record_details(entry, line));
  }
  return record;
}

std::string append_export_member(std::string_view record_text,
                                 const nlohmann::json& record,
                                 const nlohmann::json& resolutions) {
  // A trimmed JSON object ends with its closing brace.
  std::string line{record_text.substr(0, record_text.size() - 1)};
  if (!record.empty()) {
    line.push_back(',');
  }
  line.push_back('"');
  line.append(kExportMember);
  line.append("\":");
  line.append(resolutions.dump());
  line.push_back('}');
  return line;
}

}  // namespace package_export
