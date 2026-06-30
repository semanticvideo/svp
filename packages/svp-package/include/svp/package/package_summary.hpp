#pragma once

#include "svp/package/package_contract.hpp"
#include "svp/package/package_probe.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svp::package {

struct PackageJsonFileSummary {
  bool present = false;
  bool readable = false;
  bool parsed = false;
  std::string error_message;
  std::string schema_version;
};

struct PackageJsonlCountSummary {
  bool present = false;
  bool readable = false;
  std::uint64_t record_count = 0;
  std::string error_message;
};

struct PackageRequiredItemSummary {
  std::string path;
  PackageLayoutRequirementKind kind = PackageLayoutRequirementKind::required_entry;
  PackageLayoutRequirementArea area = PackageLayoutRequirementArea::core;
  bool present = false;
};

struct ManifestSummary {
  PackageJsonFileSummary file;
  std::string svp_version;
  std::string package_id;
  std::string created_utc;
  std::string primary_media_id;
  std::string timebase_unit;
  std::string timebase_origin;
  std::string canonical_raster;
};

struct TextSummary {
  PackageJsonlCountSummary text_regions;
  PackageJsonlCountSummary text_observations;
  PackageJsonlCountSummary numeric_values;
  PackageJsonFileSummary absence;
  std::string ocr_required;
  std::string ocr_completed;
  std::string text_region_count;
  std::string text_observation_count;
  std::string numeric_value_count;
  std::string absence_reason;
};

struct ColorSummary {
  PackageJsonlCountSummary color_observations;
  PackageJsonFileSummary summary;
  PackageJsonFileSummary absence;
  std::string color_observation_count;
  std::string color_space;
  std::string color_bucket_registry_version;
  std::string color_required;
  std::string color_completed;
  std::string absence_reason;
};

struct IndexManifestSummary {
  PackageJsonFileSummary file;
  bool sqlite_present = false;
  std::string index_schema_version;
  std::string sqlite_file;
  std::string logical_row_stream_version;
  std::string table_count;
  std::string row_count;
  bool has_created_from = false;
};

struct ValidationReportSummary {
  PackageJsonFileSummary file;
  std::string status;
  std::string core_status;
  std::string authenticity_status;
};

struct MediaBindingSummary {
  PackageJsonFileSummary file;
  std::string binding_id;
  std::string binding_contract;
  std::string verification_state;
  std::string media_id;
  std::string size_bytes;
  std::string duration_us;
  std::string container_format;
  std::string blake3_state;
  std::string blake3_hash;
  std::string original_filename_hint;
};

struct SvpiSummary {
  bool is_svpi = false;
  std::string svpi_version;
  std::string media_binding_ref;
  std::string primary_media_binding_id;
  MediaBindingSummary media_binding;
  bool has_media_original = false;
};

struct PackageSummary {
  PackageProbe probe;
  bool layout_readable = false;
  std::string layout_error_message;
  std::uint64_t entry_count = 0;
  std::vector<std::string> root_entries;
  std::vector<std::string> invalid_entry_paths;
  std::vector<PackageRequiredItemSummary> required_items;
  ManifestSummary manifest;
  TextSummary text;
  ColorSummary colors;
  IndexManifestSummary index;
  ValidationReportSummary validation_report;
  SvpiSummary svpi;
};

[[nodiscard]] PackageSummary read_package_summary(const std::filesystem::path& path);

}  // namespace svp::package
