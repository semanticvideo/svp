#include "index_validation.hpp"

#include "svp/package/index_logical_rows.hpp"
#include "json_schema_subset.hpp"
#include "sqlite_index_access.hpp"
#include "ocr_color_index_consistency.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace svp::validation {
namespace {

constexpr std::string_view kIndexManifestEntry = "index/index_manifest.json";
constexpr std::string_view kIndexSqliteEntry = "index/index.sqlite";
constexpr std::string_view kBlake3PatternPrefix = "blake3:";
constexpr std::size_t kBlake3HexLength = 64;

std::string package_entry_path(std::string_view entry) {
  return "/" + std::string{entry};
}

nlohmann::json read_json_entry(const std::filesystem::path& package_path,
                               std::string_view entry) {
  const auto result =
      svp::package::read_package_entry(package_path, std::string{entry});
  if (!result.has_value()) {
    throw std::runtime_error(result.error_message());
  }

  return nlohmann::json::parse(result.value());
}

nlohmann::json read_json_file(const std::filesystem::path& path) {
  std::ifstream input{path};
  if (!input) {
    throw std::runtime_error("could not open JSON file: " + path.string());
  }

  nlohmann::json value;
  input >> value;
  return value;
}

std::string schema_issue_message(const JsonSchemaIssue& issue) {
  return issue.path + ": " + issue.message;
}

bool is_lower_hex(char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool valid_blake3_digest(const nlohmann::json& value) {
  if (!value.is_string()) {
    return false;
  }

  const auto digest = value.get<std::string>();
  if (digest.size() != kBlake3PatternPrefix.size() + kBlake3HexLength ||
      digest.rfind(kBlake3PatternPrefix, 0) != 0) {
    return false;
  }

  for (auto index = kBlake3PatternPrefix.size(); index < digest.size(); ++index) {
    if (!is_lower_hex(digest[index])) {
      return false;
    }
  }

  return true;
}

void add_manifest_issue(ValidationReport& report,
                        const ValidationCodeRegistry& registry,
                        std::string message) {
  add_finding(report,
              make_finding(registry, kCodeIndexManifestInvalid,
                           package_entry_path(kIndexManifestEntry), std::move(message)));
}

void validate_exact_manifest_fields(ValidationReport& report,
                                    const ValidationCodeRegistry& registry,
                                    const nlohmann::json& manifest) {
  const auto expect_string = [&](std::string_view field, std::string_view expected) {
    const auto iterator = manifest.find(std::string{field});
    if (iterator == manifest.end() || !iterator->is_string()) {
      return;
    }

    if (iterator->get<std::string>() != expected) {
      add_manifest_issue(report, registry,
                         std::string{field} + " must be " + std::string{expected} + ".");
    }
  };

  expect_string("schema_version", "svp-index-manifest-v1");
  expect_string("sqlite_file", "index/index.sqlite");
  expect_string("logical_row_stream_version", "svp-logical-row-stream-v1");

  const auto index_schema_version = manifest.find("index_schema_version");
  if (index_schema_version != manifest.end() && index_schema_version->is_string() &&
      index_schema_version->get<std::string>().empty()) {
    add_manifest_issue(report, registry, "index_schema_version must not be empty.");
  }

  for (const auto* field : {"sqlite_file_blake3", "logical_rows_blake3"}) {
    const auto iterator = manifest.find(field);
    if (iterator != manifest.end() && !valid_blake3_digest(*iterator)) {
      add_manifest_issue(report, registry,
                         std::string{field} + " must be a blake3 digest.");
    }
  }
}

void validate_manifest_schema(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const nlohmann::json& manifest,
                              const std::filesystem::path& schema_root) {
  const auto schema = read_json_file(schema_root / "index-manifest.schema.json");
  for (const auto& issue : validate_json_schema_subset(manifest, schema)) {
    add_manifest_issue(report, registry, schema_issue_message(issue));
  }
}

std::set<std::string_view> required_index_tables() {
  return {
      "binary_blocks",
      "color_bucket_coverage",
      "color_observations",
      "color_targets",
      "numeric_values",
      "objects",
      "relationships",
      "svp_meta",
      "temporal_spans",
      "text_fts",
      "text_observations",
      "text_regions",
      "vector_index",
  };
}

void validate_required_tables(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const std::set<std::string>& table_names) {
  for (const auto table : required_index_tables()) {
    if (!table_names.contains(std::string{table})) {
      add_finding(report, make_finding(registry, kCodeIndexSchemaInvalid,
                                       package_entry_path(kIndexSqliteEntry),
                                       "Required SQLite table is missing: " +
                                           std::string{table} + "."));
    }
  }
}

void validate_table_count(ValidationReport& report,
                          const ValidationCodeRegistry& registry,
                          const nlohmann::json& manifest,
                          const svp::package::LogicalRowStreamSummary& stream) {
  const auto iterator = manifest.find("table_count");
  if (iterator == manifest.end() || !iterator->is_number_integer() ||
      iterator->get<std::int64_t>() < 0) {
    return;
  }

  const auto expected = static_cast<std::uint64_t>(iterator->get<std::int64_t>());
  if (expected != stream.table_count) {
    add_finding(report,
                make_finding(registry, kCodeIndexLogicalMismatch,
                             package_entry_path(kIndexManifestEntry),
                             "table_count does not match the canonical logical row stream."));
  }
}

void validate_row_count(ValidationReport& report,
                        const ValidationCodeRegistry& registry,
                        const nlohmann::json& manifest,
                        const svp::package::LogicalRowStreamSummary& stream) {
  const auto iterator = manifest.find("row_count");
  if (iterator == manifest.end() || !iterator->is_number_integer() ||
      iterator->get<std::int64_t>() < 0) {
    return;
  }

  const auto expected = static_cast<std::uint64_t>(iterator->get<std::int64_t>());
  if (expected != stream.row_count) {
    add_finding(report,
                make_finding(registry, kCodeIndexLogicalMismatch,
                             package_entry_path(kIndexManifestEntry),
                             "row_count does not match the canonical logical row stream."));
  }
}

void validate_logical_rows_blake3(ValidationReport& report,
                                  const ValidationCodeRegistry& registry,
                                  const nlohmann::json& manifest,
                                  const svp::package::LogicalRowStreamSummary& stream) {
  const auto iterator = manifest.find("logical_rows_blake3");
  if (iterator == manifest.end() || !iterator->is_string()) {
    return;
  }

  const auto expected = iterator->get<std::string>();
  if (expected != stream.blake3) {
    add_finding(report,
                make_finding(registry, kCodeIndexLogicalMismatch,
                             package_entry_path(kIndexManifestEntry),
                             "logical_rows_blake3 does not match the canonical SQLite "
                             "logical row stream: expected " +
                                 expected + ", actual " + stream.blake3 + "."));
  }
}

std::optional<nlohmann::json> validate_index_manifest(
    ValidationReport& report,
    const ValidationCodeRegistry& registry,
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout,
    const std::filesystem::path& schema_root) {
  if (!layout.has_entry(std::string{kIndexManifestEntry})) {
    add_manifest_issue(report, registry, "index/index_manifest.json is absent.");
    return std::nullopt;
  }

  try {
    auto manifest = read_json_entry(package_path, kIndexManifestEntry);
    validate_manifest_schema(report, registry, manifest, schema_root);
    validate_exact_manifest_fields(report, registry, manifest);
    return manifest;
  } catch (const std::exception& error) {
    add_manifest_issue(report, registry, error.what());
    return std::nullopt;
  }
}

std::optional<svp::package::LogicalRowStreamSummary> validate_sqlite_index(
    ValidationReport& report,
    const ValidationCodeRegistry& registry,
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout) {
  if (!layout.has_entry(std::string{kIndexSqliteEntry})) {
    add_finding(report, make_finding(registry, kCodeIndexSchemaInvalid,
                                     package_entry_path(kIndexSqliteEntry),
                                     "index/index.sqlite is absent."));
    return std::nullopt;
  }

  try {
    auto sqlite_temp = extract_package_entry_to_temp_file(package_path, kIndexSqliteEntry);
    auto database = open_read_only_database(sqlite_temp.path());
    auto table_names = read_user_table_names(*database);
    validate_required_tables(report, registry, table_names);
    add_ocr_color_index_consistency_findings(report, registry, package_path, layout,
                                             *database);
    return svp::package::compute_logical_row_stream_summary(*database, table_names);
  } catch (const std::exception& error) {
    add_finding(report, make_finding(registry, kCodeIndexSchemaInvalid,
                                     package_entry_path(kIndexSqliteEntry), error.what()));
    return std::nullopt;
  }
}

}  // namespace

void add_index_findings(ValidationReport& report,
                        const ValidationCodeRegistry& registry,
                        const std::filesystem::path& package_path,
                        const svp::package::PackageLayout& layout,
                        const std::filesystem::path& schema_root) {
  const auto manifest =
      validate_index_manifest(report, registry, package_path, layout, schema_root);
  const auto stream = validate_sqlite_index(report, registry, package_path, layout);
  if (manifest.has_value() && stream.has_value()) {
    validate_table_count(report, registry, manifest.value(), stream.value());
    validate_row_count(report, registry, manifest.value(), stream.value());
    validate_logical_rows_blake3(report, registry, manifest.value(), stream.value());
  }
}

}  // namespace svp::validation
