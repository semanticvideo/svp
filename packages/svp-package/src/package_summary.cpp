#include "svp/package/package_summary.hpp"

#include "svp/package/package_layout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace svp::package {
namespace {

bool is_blank_line(std::string_view line) {
  return std::ranges::all_of(line, [](unsigned char character) {
    return std::isspace(character) != 0;
  });
}

std::string scalar_to_string(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_float()) {
    return value.dump();
  }
  return {};
}

std::string object_scalar(const nlohmann::json& object, std::string_view key) {
  if (!object.is_object()) {
    return {};
  }

  const auto iterator = object.find(key);
  if (iterator == object.end()) {
    return {};
  }

  return scalar_to_string(*iterator);
}

std::string nested_object_scalar(const nlohmann::json& object,
                                 std::string_view object_key,
                                 std::string_view scalar_key) {
  if (!object.is_object()) {
    return {};
  }

  const auto nested = object.find(object_key);
  if (nested == object.end() || !nested->is_object()) {
    return {};
  }

  return object_scalar(*nested, scalar_key);
}

std::string raster_label(const nlohmann::json& manifest) {
  if (!manifest.is_object()) {
    return {};
  }

  const auto raster = manifest.find("canonical_analysis_raster");
  if (raster == manifest.end() || !raster->is_object()) {
    return {};
  }

  const auto width = object_scalar(*raster, "width");
  const auto height = object_scalar(*raster, "height");
  if (width.empty() || height.empty()) {
    return {};
  }

  return width + "x" + height;
}

nlohmann::json read_json_entry(const std::filesystem::path& path,
                               const PackageLayout& layout,
                               const std::string& entry,
                               PackageJsonFileSummary& file) {
  file.present = layout.has_entry(entry);
  if (!file.present) {
    return nlohmann::json{};
  }

  const auto read_result = read_package_entry(path, entry);
  if (!read_result.has_value()) {
    file.error_message = read_result.error_message();
    return nlohmann::json{};
  }

  file.readable = true;
  nlohmann::json parsed = nlohmann::json::parse(read_result.value(), nullptr, false);
  if (parsed.is_discarded()) {
    file.error_message = "JSON parse failed";
    return nlohmann::json{};
  }

  file.parsed = true;
  file.schema_version = object_scalar(parsed, "schema_version");
  return parsed;
}

PackageJsonlCountSummary count_jsonl_records(const std::filesystem::path& path,
                                             const PackageLayout& layout,
                                             const std::string& entry) {
  PackageJsonlCountSummary count;
  count.present = layout.has_entry(entry);
  if (!count.present) {
    return count;
  }

  const auto read_result = read_package_entry(path, entry);
  if (!read_result.has_value()) {
    count.error_message = read_result.error_message();
    return count;
  }

  count.readable = true;
  std::string_view content{read_result.value()};
  while (!content.empty()) {
    const auto newline = content.find('\n');
    const auto line = content.substr(0, newline);
    if (!is_blank_line(line)) {
      ++count.record_count;
    }

    if (newline == std::string_view::npos) {
      break;
    }
    content.remove_prefix(newline + 1);
  }

  return count;
}

void populate_required_items(const PackageLayout& layout, PackageSummary& summary) {
  for (const auto& requirement : required_package_layout()) {
    PackageRequiredItemSummary item;
    item.path = std::string{requirement.path};
    item.kind = requirement.kind;
    item.area = requirement.area;
    item.present = requirement.kind == PackageLayoutRequirementKind::required_entry
                       ? layout.has_entry(item.path)
                       : layout.has_top_level_section(item.path);
    summary.required_items.push_back(std::move(item));
  }
}

void populate_manifest(const std::filesystem::path& path,
                       const PackageLayout& layout,
                       PackageSummary& summary) {
  const auto manifest = read_json_entry(path, layout, "manifest.json", summary.manifest.file);
  if (!summary.manifest.file.parsed) {
    return;
  }

  summary.manifest.svp_version = object_scalar(manifest, "svp_version");
  summary.manifest.package_id = object_scalar(manifest, "package_id");
  summary.manifest.created_utc = object_scalar(manifest, "created_utc");
  summary.manifest.primary_media_id = object_scalar(manifest, "primary_media_id");
  summary.manifest.timebase_unit = nested_object_scalar(manifest, "timebase", "unit");
  summary.manifest.timebase_origin = nested_object_scalar(manifest, "timebase", "origin");
  summary.manifest.canonical_raster = raster_label(manifest);
}

void populate_text(const std::filesystem::path& path,
                   const PackageLayout& layout,
                   PackageSummary& summary) {
  summary.text.text_regions = count_jsonl_records(path, layout, "text/text_regions.jsonl");
  summary.text.text_observations =
      count_jsonl_records(path, layout, "text/text_observations.jsonl");
  summary.text.numeric_values = count_jsonl_records(path, layout, "text/numeric_values.jsonl");

  const auto absence =
      read_json_entry(path, layout, "text/text_absence.json", summary.text.absence);
  if (!summary.text.absence.parsed) {
    return;
  }

  summary.text.ocr_required = object_scalar(absence, "ocr_required");
  summary.text.ocr_completed = object_scalar(absence, "ocr_completed");
  summary.text.absence_reason = object_scalar(absence, "reason");
}

void populate_colors(const std::filesystem::path& path,
                     const PackageLayout& layout,
                     PackageSummary& summary) {
  summary.colors.color_observations =
      count_jsonl_records(path, layout, "colors/color_observations.jsonl");

  const auto color_summary =
      read_json_entry(path, layout, "colors/color_summary.json", summary.colors.summary);
  if (summary.colors.summary.parsed) {
    summary.colors.color_observation_count =
        object_scalar(color_summary, "color_observation_count");
    summary.colors.color_space = object_scalar(color_summary, "color_space");
    summary.colors.color_bucket_registry_version =
        object_scalar(color_summary, "color_bucket_registry_version");
  }

  const auto absence =
      read_json_entry(path, layout, "colors/color_absence.json", summary.colors.absence);
  if (!summary.colors.absence.parsed) {
    return;
  }

  summary.colors.color_required = object_scalar(absence, "color_required");
  summary.colors.color_completed = object_scalar(absence, "color_completed");
  summary.colors.absence_reason = object_scalar(absence, "reason");
}

void populate_index(const std::filesystem::path& path,
                    const PackageLayout& layout,
                    PackageSummary& summary) {
  summary.index.sqlite_present = layout.has_entry("index/index.sqlite");

  const auto index_manifest =
      read_json_entry(path, layout, "index/index_manifest.json", summary.index.file);
  if (!summary.index.file.parsed) {
    return;
  }

  summary.index.index_schema_version = object_scalar(index_manifest, "index_schema_version");
  summary.index.sqlite_file = object_scalar(index_manifest, "sqlite_file");
  summary.index.logical_row_stream_version =
      object_scalar(index_manifest, "logical_row_stream_version");
  summary.index.table_count = object_scalar(index_manifest, "table_count");
  summary.index.row_count = object_scalar(index_manifest, "row_count");
  const auto created_from = index_manifest.find("created_from");
  summary.index.has_created_from = created_from != index_manifest.end() && created_from->is_object();
}

void populate_validation_report(const std::filesystem::path& path,
                                const PackageLayout& layout,
                                PackageSummary& summary) {
  const auto validation_report = read_json_entry(
      path, layout, "provenance/validation.json", summary.validation_report.file);
  if (!summary.validation_report.file.parsed) {
    return;
  }

  summary.validation_report.status = object_scalar(validation_report, "status");
  summary.validation_report.core_status = object_scalar(validation_report, "core_status");
  summary.validation_report.authenticity_status =
      object_scalar(validation_report, "authenticity_status");
}

}  // namespace

PackageSummary read_package_summary(const std::filesystem::path& path) {
  PackageSummary summary;
  summary.probe = probe_package(path);

  const auto layout_result = read_package_layout(path);
  if (!layout_result.has_value()) {
    summary.layout_error_message = layout_result.error_message();
    return summary;
  }

  const auto& layout = layout_result.value();
  summary.layout_readable = true;
  summary.entry_count = layout.entries.size();
  summary.root_entries.assign(layout.root_entries.begin(), layout.root_entries.end());
  summary.invalid_entry_paths = layout.invalid_entry_paths;

  populate_required_items(layout, summary);
  populate_manifest(path, layout, summary);
  populate_text(path, layout, summary);
  populate_colors(path, layout, summary);
  populate_index(path, layout, summary);
  populate_validation_report(path, layout, summary);

  return summary;
}

}  // namespace svp::package
