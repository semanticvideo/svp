#include "text_record_validation.hpp"

#include "json_schema_subset.hpp"
#include "record_file_reader.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace svp::validation {
namespace {

struct RasterExtent {
  int width = 0;
  int height = 0;
};

struct TextIds {
  std::set<std::string> region_ids;
  std::set<std::string> observation_ids;
};

std::string package_entry_path(std::string_view entry) {
  return "/" + std::string{entry};
}

std::string record_path(std::string_view entry, std::size_t line) {
  return package_entry_path(entry) + ":" + std::to_string(line);
}

std::string schema_issue_message(const JsonSchemaIssue& issue) {
  return issue.path + ": " + issue.message;
}

void add_schema_findings(ValidationReport& report,
                         const ValidationCodeRegistry& registry,
                         std::string_view code,
                         std::string_view entry,
                         const JsonLineRecord& record,
                         const nlohmann::json& schema) {
  for (const auto& issue : validate_json_schema_subset(record.value, schema)) {
    add_finding(report, make_finding(registry, code, record_path(entry, record.line),
                                     schema_issue_message(issue)));
  }
}

bool has_entry(const svp::package::PackageLayout& layout, const std::string& entry) {
  return layout.has_entry(entry);
}

std::optional<RasterExtent> read_manifest_raster(const std::filesystem::path& package_path,
                                                 const svp::package::PackageLayout& layout) {
  if (!has_entry(layout, "manifest.json")) {
    return std::nullopt;
  }

  const auto entry = svp::package::read_package_entry(package_path, "manifest.json");
  if (!entry.has_value()) {
    return std::nullopt;
  }

  try {
    const auto manifest = nlohmann::json::parse(entry.value());
    const auto& raster = manifest.at("canonical_analysis_raster");
    const auto width = raster.at("width").get<int>();
    const auto height = raster.at("height").get<int>();
    if (width <= 0 || height <= 0) {
      return std::nullopt;
    }
    return RasterExtent{.width = width, .height = height};
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

bool finite_number(const nlohmann::json& value) {
  return value.is_number() && std::isfinite(value.get<double>());
}

bool valid_normalized_bbox(const nlohmann::json& bbox) {
  if (!bbox.is_array() || bbox.size() != 4) {
    return false;
  }
  for (const auto& coordinate : bbox) {
    if (!finite_number(coordinate)) {
      return false;
    }
  }

  const auto x_min = bbox.at(0).get<double>();
  const auto y_min = bbox.at(1).get<double>();
  const auto x_max = bbox.at(2).get<double>();
  const auto y_max = bbox.at(3).get<double>();
  return x_min >= 0.0 && y_min >= 0.0 && x_max <= 1.0 && y_max <= 1.0 &&
         x_min < x_max && y_min < y_max;
}

bool valid_pixel_bbox(const nlohmann::json& bbox, std::optional<RasterExtent> raster) {
  if (!bbox.is_array() || bbox.size() != 4) {
    return false;
  }
  for (const auto& coordinate : bbox) {
    if (!coordinate.is_number_integer()) {
      return false;
    }
  }

  const auto x_min = bbox.at(0).get<int>();
  const auto y_min = bbox.at(1).get<int>();
  const auto x_max = bbox.at(2).get<int>();
  const auto y_max = bbox.at(3).get<int>();
  if (x_min < 0 || y_min < 0 || x_min >= x_max || y_min >= y_max) {
    return false;
  }

  if (!raster.has_value()) {
    return true;
  }

  return x_max <= raster->width && y_max <= raster->height;
}

void validate_bbox(ValidationReport& report,
                   const ValidationCodeRegistry& registry,
                   const JsonLineRecord& record,
                   std::optional<RasterExtent> raster) {
  const bool norm_ok =
      record.value.contains("bbox_norm") && valid_normalized_bbox(record.value.at("bbox_norm"));
  const bool px_ok =
      record.value.contains("bbox_px") && valid_pixel_bbox(record.value.at("bbox_px"), raster);
  if (!norm_ok || !px_ok) {
    add_finding(report, make_finding(registry, kCodeTextInvalidBoundingBox,
                                     record_path("text/text_regions.jsonl", record.line),
                                     "Text region bounding boxes are outside valid bounds."));
  }
}

void validate_region_timing(ValidationReport& report,
                            const ValidationCodeRegistry& registry,
                            const JsonLineRecord& record) {
  bool invalid = false;
  if (record.value.contains("start_us") && record.value.contains("end_us") &&
      record.value.at("start_us").is_number_integer() &&
      record.value.at("end_us").is_number_integer()) {
    invalid = record.value.at("start_us").get<long long>() >
              record.value.at("end_us").get<long long>();
  }

  if (record.value.contains("frame_start") && record.value.contains("frame_end") &&
      record.value.at("frame_start").is_number_integer() &&
      record.value.at("frame_end").is_number_integer()) {
    invalid = invalid || record.value.at("frame_start").get<long long>() >
                             record.value.at("frame_end").get<long long>();
  }

  if (invalid) {
    add_finding(report, make_finding(registry, kCodeTextInvalidTiming,
                                     record_path("text/text_regions.jsonl", record.line),
                                     "Text region timing or frame range is reversed."));
  }
}

void validate_confidence(ValidationReport& report,
                         const ValidationCodeRegistry& registry,
                         std::string_view entry,
                         const JsonLineRecord& record) {
  if (!record.value.contains("confidence")) {
    return;
  }

  const auto& confidence = record.value.at("confidence");
  if (!finite_number(confidence) || confidence.get<double>() < 0.0 ||
      confidence.get<double>() > 1.0) {
    add_finding(report, make_finding(registry, kCodeTextInvalidConfidence,
                                     record_path(entry, record.line),
                                     "Text confidence is not finite in [0,1]."));
  }
}

void validate_observation_type(ValidationReport& report,
                               const ValidationCodeRegistry& registry,
                               std::string_view entry,
                               std::string_view code,
                               const JsonLineRecord& record,
                               const OcrColorSpec& spec) {
  if (!record.value.contains("observation_type") ||
      !record.value.at("observation_type").is_string()) {
    return;
  }

  const auto observation_type = record.value.at("observation_type").get<std::string>();
  if (!spec.ocr_observation_types.contains(observation_type)) {
    add_finding(report, make_finding(registry, code, record_path(entry, record.line),
                                     "OCR observation_type is not registered."));
  }
}

void validate_normalized_text(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              std::string_view entry,
                              std::string_view code,
                              const JsonLineRecord& record) {
  if (!record.value.contains("raw_text") || !record.value.at("raw_text").is_string()) {
    return;
  }

  const auto raw_text = record.value.at("raw_text").get<std::string>();
  if (raw_text.empty()) {
    return;
  }

  if (!record.value.contains("normalized_text") ||
      !record.value.at("normalized_text").is_string() ||
      record.value.at("normalized_text").get<std::string>().empty()) {
    add_finding(report, make_finding(registry, code, record_path(entry, record.line),
                                     "normalized_text is required when raw_text is non-empty."));
  }
}

void validate_text_region_records(ValidationReport& report,
                                  const ValidationCodeRegistry& registry,
                                  const std::filesystem::path& package_path,
                                  const svp::package::PackageLayout& layout,
                                  const OcrColorSpec& spec,
                                  TextIds& ids) {
  constexpr std::string_view entry = "text/text_regions.jsonl";
  if (!has_entry(layout, std::string{entry})) {
    return;
  }

  const auto raster = read_manifest_raster(package_path, layout);
  const auto records = read_json_lines_from_package(package_path, std::string{entry});
  if (!records.has_value()) {
    add_finding(report, make_finding(registry, kCodeTextInvalidRegionRecord,
                                     package_entry_path(entry), records.error_message));
    return;
  }

  for (const auto& record : records.records) {
    add_schema_findings(report, registry, kCodeTextInvalidRegionRecord, entry, record,
                        spec.text_region_schema);
    validate_bbox(report, registry, record, raster);
    validate_region_timing(report, registry, record);
    validate_confidence(report, registry, entry, record);
    validate_observation_type(report, registry, entry, kCodeTextInvalidRegionRecord, record,
                              spec);
    if (record.value.contains("text_region_id") &&
        record.value.at("text_region_id").is_string()) {
      ids.region_ids.insert(record.value.at("text_region_id").get<std::string>());
    }
  }
}

void validate_text_observation_records(ValidationReport& report,
                                       const ValidationCodeRegistry& registry,
                                       const std::filesystem::path& package_path,
                                       const svp::package::PackageLayout& layout,
                                       const OcrColorSpec& spec,
                                       TextIds& ids) {
  constexpr std::string_view entry = "text/text_observations.jsonl";
  if (!has_entry(layout, std::string{entry})) {
    return;
  }

  const auto records = read_json_lines_from_package(package_path, std::string{entry});
  if (!records.has_value()) {
    add_finding(report, make_finding(registry, kCodeTextInvalidObservationRecord,
                                     package_entry_path(entry), records.error_message));
    return;
  }

  for (const auto& record : records.records) {
    add_schema_findings(report, registry, kCodeTextInvalidObservationRecord, entry, record,
                        spec.text_observation_schema);
    validate_confidence(report, registry, entry, record);
    validate_observation_type(report, registry, entry, kCodeTextInvalidObservationRecord,
                              record, spec);
    validate_normalized_text(report, registry, entry, kCodeTextInvalidNormalizedText, record);

    if (record.value.contains("text_region_id") &&
        record.value.at("text_region_id").is_string() &&
        !ids.region_ids.contains(record.value.at("text_region_id").get<std::string>())) {
      add_finding(report, make_finding(registry, kCodeTextInvalidReference,
                                       record_path(entry, record.line),
                                       "text_region_id does not reference a text region."));
    }

    if (record.value.contains("text_observation_id") &&
        record.value.at("text_observation_id").is_string()) {
      ids.observation_ids.insert(record.value.at("text_observation_id").get<std::string>());
    }
  }
}

void validate_numeric_value_records(ValidationReport& report,
                                    const ValidationCodeRegistry& registry,
                                    const std::filesystem::path& package_path,
                                    const svp::package::PackageLayout& layout,
                                    const OcrColorSpec& spec,
                                    const TextIds& ids) {
  constexpr std::string_view entry = "text/numeric_values.jsonl";
  if (!has_entry(layout, std::string{entry})) {
    return;
  }

  const auto records = read_json_lines_from_package(package_path, std::string{entry});
  if (!records.has_value()) {
    add_finding(report, make_finding(registry, kCodeTextInvalidNumericExtraction,
                                     package_entry_path(entry), records.error_message));
    return;
  }

  for (const auto& record : records.records) {
    add_schema_findings(report, registry, kCodeTextInvalidNumericExtraction, entry, record,
                        spec.numeric_value_schema);
    validate_confidence(report, registry, entry, record);
    validate_normalized_text(report, registry, entry, kCodeTextInvalidNumericExtraction,
                             record);

    if (record.value.contains("text_region_id") &&
        record.value.at("text_region_id").is_string() &&
        !ids.region_ids.contains(record.value.at("text_region_id").get<std::string>())) {
      add_finding(report, make_finding(registry, kCodeTextInvalidReference,
                                       record_path(entry, record.line),
                                       "text_region_id does not reference a text region."));
    }

    if (record.value.contains("text_observation_id") &&
        record.value.at("text_observation_id").is_string() &&
        !ids.observation_ids.contains(
            record.value.at("text_observation_id").get<std::string>())) {
      add_finding(report, make_finding(registry, kCodeTextInvalidReference,
                                       record_path(entry, record.line),
                                       "text_observation_id does not reference a text observation."));
    }
  }
}

}  // namespace

void add_text_record_findings(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const std::filesystem::path& package_path,
                              const svp::package::PackageLayout& layout,
                              const OcrColorSpec& spec) {
  TextIds ids;
  validate_text_region_records(report, registry, package_path, layout, spec, ids);
  validate_text_observation_records(report, registry, package_path, layout, spec, ids);
  validate_numeric_value_records(report, registry, package_path, layout, spec, ids);
}

}  // namespace svp::validation
