#include "entity_record_validation.hpp"

#include "record_file_reader.hpp"
#include "svp/package/package_layout.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace svp::validation {
namespace {

constexpr std::array<std::string_view, 5> kEntityTypes = {
    "visual_entity", "background_region", "screen_region", "face_region",
    "unknown_region"};

bool valid_entity_type(const nlohmann::json& record) {
  if (!record.contains("entity_type") || !record["entity_type"].is_string()) {
    return false;
  }
  const auto type = record["entity_type"].get<std::string>();
  return std::find(kEntityTypes.begin(), kEntityTypes.end(), type) !=
         kEntityTypes.end();
}

}  // namespace

void add_entity_record_findings(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                const std::filesystem::path& package_path,
                                const svp::package::PackageLayout& layout) {
  constexpr std::string_view kEntry = "entities/entities.jsonl";
  if (!layout.has_entry(std::string{kEntry}))
    return;
  const auto records =
      read_json_lines_from_package(package_path, std::string{kEntry});
  if (!records.has_value()) {
    add_finding(report,
                make_finding(registry, kCodeInvalidEntityRecord,
                             std::string{kEntry},
                             "Entity records are not valid JSON Lines: " +
                                 records.error_message));
    return;
  }
  for (const auto& record : records.records) {
    if (valid_entity_type(record.value))
      continue;
    add_finding(
        report,
        make_finding(registry, kCodeInvalidEntityRecord,
                     std::string{kEntry} + ":" + std::to_string(record.line),
                     "entity_type must be one of the structural types defined "
                     "by RC2 Section 13.2."));
  }
}

}  // namespace svp::validation
