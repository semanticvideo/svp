#include "ocr_color_index_consistency.hpp"

#include "record_file_reader.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace svp::validation {
namespace {

constexpr std::string_view kIndexSqliteEntry = "index/index.sqlite";
constexpr double kSqliteRealTolerance = 0.000000001;

struct StatementDeleter {
  void operator()(sqlite3_stmt* statement) const noexcept {
    if (statement != nullptr) {
      sqlite3_finalize(statement);
    }
  }
};

struct TextRegionRecord {
  std::size_t line = 0;
  std::optional<std::int64_t> start_us;
  std::optional<std::int64_t> end_us;
  std::optional<std::string> shot_id;
  std::optional<std::string> scene_id;
};

struct TextObservationRecord {
  std::size_t line = 0;
  std::string text_region_id;
  std::string raw_text;
  std::string normalized_text;
  std::optional<std::string> layout_class;
};

struct NumericValueRecord {
  std::size_t line = 0;
  std::string text_observation_id;
  std::string text_region_id;
  std::string numeric_value;
  std::string raw_text;
  std::string normalized_text;
};

struct ColorObservationRecord {
  std::size_t line = 0;
  std::string target_type;
  std::string target_id;
  std::string dominant_bucket;
  std::map<std::string, double> bucket_coverage;
};

using TextRegionMap = std::unordered_map<std::string, TextRegionRecord>;
using TextObservationMap = std::unordered_map<std::string, TextObservationRecord>;
using NumericValueMap = std::unordered_map<std::string, NumericValueRecord>;
using ColorObservationMap = std::unordered_map<std::string, ColorObservationRecord>;

std::string package_entry_path(std::string_view entry) {
  return "/" + std::string{entry};
}

std::string record_path(std::string_view entry, std::size_t line) {
  return package_entry_path(entry) + ":" + std::to_string(line);
}

std::string index_table_path(std::string_view table) {
  return package_entry_path(kIndexSqliteEntry) + "#" + std::string{table};
}

std::optional<std::string> json_optional_string(const nlohmann::json& value,
                                                std::string_view key) {
  const auto iterator = value.find(std::string{key});
  if (iterator == value.end() || iterator->is_null()) {
    return std::nullopt;
  }
  if (!iterator->is_string()) {
    return std::nullopt;
  }
  return iterator->get<std::string>();
}

std::optional<std::int64_t> json_optional_int(const nlohmann::json& value,
                                              std::string_view key) {
  const auto iterator = value.find(std::string{key});
  if (iterator == value.end() || iterator->is_null()) {
    return std::nullopt;
  }
  if (!iterator->is_number_integer()) {
    return std::nullopt;
  }
  return iterator->get<std::int64_t>();
}

std::optional<std::string> sqlite_optional_text(sqlite3_stmt& statement, int column) {
  if (sqlite3_column_type(&statement, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  const auto* text = sqlite3_column_text(&statement, column);
  if (text == nullptr) {
    return std::nullopt;
  }
  return std::string{reinterpret_cast<const char*>(text)};
}

std::string sqlite_required_text(sqlite3_stmt& statement, int column) {
  return sqlite_optional_text(statement, column).value_or("");
}

std::optional<std::int64_t> sqlite_optional_int(sqlite3_stmt& statement, int column) {
  if (sqlite3_column_type(&statement, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  return sqlite3_column_int64(&statement, column);
}

std::unique_ptr<sqlite3_stmt, StatementDeleter> prepare(sqlite3& database,
                                                        std::string_view query) {
  sqlite3_stmt* raw_statement = nullptr;
  if (sqlite3_prepare_v2(&database, query.data(), static_cast<int>(query.size()),
                         &raw_statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(&database));
  }
  return std::unique_ptr<sqlite3_stmt, StatementDeleter>{raw_statement};
}

std::set<std::string> table_columns(sqlite3& database, std::string_view table) {
  auto statement = prepare(database, "PRAGMA table_info(" + std::string{table} + ")");
  std::set<std::string> columns;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      return columns;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }
    columns.insert(sqlite_required_text(*statement, 1));
  }
}

bool has_columns(ValidationReport& report,
                 const ValidationCodeRegistry& registry,
                 sqlite3& database,
                 std::string_view table,
                 const std::vector<std::string_view>& expected_columns) {
  const auto columns = table_columns(database, table);
  bool complete = true;
  for (const auto column : expected_columns) {
    if (!columns.contains(std::string{column})) {
      complete = false;
      add_finding(report, make_finding(registry, kCodeIndexSchemaInvalid,
                                       index_table_path(table),
                                       "Required SQLite column is missing: " +
                                           std::string{column} + "."));
    }
  }
  return complete;
}

bool text_region_values_match(const TextRegionRecord& package_record,
                              const TextRegionRecord& index_record) {
  return package_record.start_us == index_record.start_us &&
         package_record.end_us == index_record.end_us &&
         package_record.shot_id == index_record.shot_id &&
         package_record.scene_id == index_record.scene_id;
}

bool text_observation_values_match(const TextObservationRecord& package_record,
                                   const TextObservationRecord& index_record) {
  return package_record.text_region_id == index_record.text_region_id &&
         package_record.raw_text == index_record.raw_text &&
         package_record.normalized_text == index_record.normalized_text &&
         package_record.layout_class == index_record.layout_class;
}

bool numeric_value_values_match(const NumericValueRecord& package_record,
                                const NumericValueRecord& index_record) {
  return package_record.text_observation_id == index_record.text_observation_id &&
         package_record.text_region_id == index_record.text_region_id &&
         package_record.numeric_value == index_record.numeric_value &&
         package_record.raw_text == index_record.raw_text &&
         package_record.normalized_text == index_record.normalized_text;
}

bool color_observation_values_match(const ColorObservationRecord& package_record,
                                    const ColorObservationRecord& index_record) {
  return package_record.target_type == index_record.target_type &&
         package_record.target_id == index_record.target_id &&
         package_record.dominant_bucket == index_record.dominant_bucket;
}

bool coverage_values_match(const std::map<std::string, double>& package_coverage,
                           const std::map<std::string, double>& index_coverage) {
  if (package_coverage.size() != index_coverage.size()) {
    return false;
  }

  for (const auto& [bucket_id, package_value] : package_coverage) {
    const auto iterator = index_coverage.find(bucket_id);
    if (iterator == index_coverage.end()) {
      return false;
    }
    if (std::abs(package_value - iterator->second) > kSqliteRealTolerance) {
      return false;
    }
  }

  return true;
}

void add_text_index_issue(ValidationReport& report,
                          const ValidationCodeRegistry& registry,
                          std::string path,
                          std::string message) {
  add_finding(report,
              make_finding(registry, kCodeTextIndexMismatch, std::move(path),
                           std::move(message)));
}

void add_color_index_issue(ValidationReport& report,
                           const ValidationCodeRegistry& registry,
                           std::string path,
                           std::string message) {
  add_finding(report,
              make_finding(registry, kCodeColorIndexMismatch, std::move(path),
                           std::move(message)));
}

TextRegionMap read_package_text_regions(const std::filesystem::path& package_path,
                                         const svp::package::PackageLayout& layout) {
  constexpr std::string_view entry = "text/text_regions.jsonl";
  TextRegionMap records;
  if (!layout.has_entry(std::string{entry})) {
    return records;
  }

  const auto lines = read_json_lines_from_package(package_path, std::string{entry});
  if (!lines.has_value()) {
    return records;
  }

  for (const auto& line : lines.records) {
    const auto id = json_optional_string(line.value, "text_region_id");
    if (!id.has_value()) {
      continue;
    }
    records[*id] = TextRegionRecord{
        .line = line.line,
        .start_us = json_optional_int(line.value, "start_us"),
        .end_us = json_optional_int(line.value, "end_us"),
        .shot_id = json_optional_string(line.value, "shot_id"),
        .scene_id = json_optional_string(line.value, "scene_id"),
    };
  }

  return records;
}

TextObservationMap read_package_text_observations(
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout) {
  constexpr std::string_view entry = "text/text_observations.jsonl";
  TextObservationMap records;
  if (!layout.has_entry(std::string{entry})) {
    return records;
  }

  const auto lines = read_json_lines_from_package(package_path, std::string{entry});
  if (!lines.has_value()) {
    return records;
  }

  for (const auto& line : lines.records) {
    const auto id = json_optional_string(line.value, "text_observation_id");
    const auto text_region_id = json_optional_string(line.value, "text_region_id");
    const auto raw_text = json_optional_string(line.value, "raw_text");
    const auto normalized_text = json_optional_string(line.value, "normalized_text");
    if (!id.has_value() || !text_region_id.has_value() || !raw_text.has_value() ||
        !normalized_text.has_value()) {
      continue;
    }
    records[*id] = TextObservationRecord{
        .line = line.line,
        .text_region_id = *text_region_id,
        .raw_text = *raw_text,
        .normalized_text = *normalized_text,
        .layout_class = json_optional_string(line.value, "layout_class"),
    };
  }

  return records;
}

NumericValueMap read_package_numeric_values(const std::filesystem::path& package_path,
                                            const svp::package::PackageLayout& layout) {
  constexpr std::string_view entry = "text/numeric_values.jsonl";
  NumericValueMap records;
  if (!layout.has_entry(std::string{entry})) {
    return records;
  }

  const auto lines = read_json_lines_from_package(package_path, std::string{entry});
  if (!lines.has_value()) {
    return records;
  }

  for (const auto& line : lines.records) {
    const auto id = json_optional_string(line.value, "numeric_value_id");
    const auto text_observation_id = json_optional_string(line.value, "text_observation_id");
    const auto text_region_id = json_optional_string(line.value, "text_region_id");
    const auto numeric_value = json_optional_string(line.value, "numeric_value");
    const auto raw_text = json_optional_string(line.value, "raw_text");
    const auto normalized_text = json_optional_string(line.value, "normalized_text");
    if (!id.has_value() || !text_observation_id.has_value() ||
        !text_region_id.has_value() || !numeric_value.has_value() ||
        !raw_text.has_value() || !normalized_text.has_value()) {
      continue;
    }
    records[*id] = NumericValueRecord{
        .line = line.line,
        .text_observation_id = *text_observation_id,
        .text_region_id = *text_region_id,
        .numeric_value = *numeric_value,
        .raw_text = *raw_text,
        .normalized_text = *normalized_text,
    };
  }

  return records;
}

ColorObservationMap read_package_color_observations(
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout) {
  constexpr std::string_view entry = "colors/color_observations.jsonl";
  ColorObservationMap records;
  if (!layout.has_entry(std::string{entry})) {
    return records;
  }

  const auto lines = read_json_lines_from_package(package_path, std::string{entry});
  if (!lines.has_value()) {
    return records;
  }

  for (const auto& line : lines.records) {
    const auto id = json_optional_string(line.value, "color_observation_id");
    const auto target_type = json_optional_string(line.value, "target_type");
    const auto target_id = json_optional_string(line.value, "target_id");
    const auto dominant_bucket = json_optional_string(line.value, "dominant_bucket");
    if (!id.has_value() || !target_type.has_value() || !target_id.has_value() ||
        !dominant_bucket.has_value() || !line.value.contains("bucket_coverage") ||
        !line.value.at("bucket_coverage").is_object()) {
      continue;
    }

    std::map<std::string, double> bucket_coverage;
    bool usable = true;
    for (const auto& [bucket_id, coverage] : line.value.at("bucket_coverage").items()) {
      if (!coverage.is_number() || !std::isfinite(coverage.get<double>())) {
        usable = false;
        break;
      }
      bucket_coverage[bucket_id] = coverage.get<double>();
    }
    if (!usable) {
      continue;
    }

    records[*id] = ColorObservationRecord{
        .line = line.line,
        .target_type = *target_type,
        .target_id = *target_id,
        .dominant_bucket = *dominant_bucket,
        .bucket_coverage = std::move(bucket_coverage),
    };
  }

  return records;
}

TextRegionMap read_index_text_regions(sqlite3& database) {
  auto statement =
      prepare(database,
              "SELECT text_region_id, start_us, end_us, shot_id, scene_id "
              "FROM text_regions");
  TextRegionMap records;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      return records;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }
    records[sqlite_required_text(*statement, 0)] = TextRegionRecord{
        .start_us = sqlite_optional_int(*statement, 1),
        .end_us = sqlite_optional_int(*statement, 2),
        .shot_id = sqlite_optional_text(*statement, 3),
        .scene_id = sqlite_optional_text(*statement, 4),
    };
  }
}

TextObservationMap read_index_text_observations(sqlite3& database) {
  auto statement =
      prepare(database,
              "SELECT text_observation_id, text_region_id, raw_text, normalized_text, "
              "layout_class FROM text_observations");
  TextObservationMap records;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      return records;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }
    records[sqlite_required_text(*statement, 0)] = TextObservationRecord{
        .text_region_id = sqlite_required_text(*statement, 1),
        .raw_text = sqlite_required_text(*statement, 2),
        .normalized_text = sqlite_required_text(*statement, 3),
        .layout_class = sqlite_optional_text(*statement, 4),
    };
  }
}

NumericValueMap read_index_numeric_values(sqlite3& database) {
  auto statement =
      prepare(database,
              "SELECT numeric_value_id, text_observation_id, text_region_id, "
              "numeric_value, raw_text, normalized_text FROM numeric_values");
  NumericValueMap records;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      return records;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }
    records[sqlite_required_text(*statement, 0)] = NumericValueRecord{
        .text_observation_id = sqlite_required_text(*statement, 1),
        .text_region_id = sqlite_required_text(*statement, 2),
        .numeric_value = sqlite_required_text(*statement, 3),
        .raw_text = sqlite_required_text(*statement, 4),
        .normalized_text = sqlite_required_text(*statement, 5),
    };
  }
}

std::set<std::pair<std::string, std::string>> read_text_fts_rows(sqlite3& database) {
  auto statement = prepare(database,
                           "SELECT object_id, text FROM text_fts "
                           "WHERE object_type = 'text_observation'");
  std::set<std::pair<std::string, std::string>> rows;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      return rows;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }
    rows.emplace(sqlite_required_text(*statement, 0), sqlite_required_text(*statement, 1));
  }
}

ColorObservationMap read_index_color_observations(sqlite3& database) {
  auto statement =
      prepare(database,
              "SELECT color_observation_id, target_type, target_id, dominant_bucket "
              "FROM color_observations");
  ColorObservationMap records;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      return records;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }
    records[sqlite_required_text(*statement, 0)] = ColorObservationRecord{
        .target_type = sqlite_required_text(*statement, 1),
        .target_id = sqlite_required_text(*statement, 2),
        .dominant_bucket = sqlite_required_text(*statement, 3),
    };
  }
}

std::set<std::string> read_color_target_rows(sqlite3& database) {
  auto statement =
      prepare(database,
              "SELECT color_observation_id, target_type, target_id FROM color_targets");
  std::set<std::string> rows;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      return rows;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }
    rows.insert(sqlite_required_text(*statement, 0) + "\n" +
                sqlite_required_text(*statement, 1) + "\n" +
                sqlite_required_text(*statement, 2));
  }
}

std::unordered_map<std::string, std::map<std::string, double>> read_color_bucket_rows(
    sqlite3& database) {
  auto statement =
      prepare(database,
              "SELECT color_observation_id, bucket_id, coverage "
              "FROM color_bucket_coverage");
  std::unordered_map<std::string, std::map<std::string, double>> rows;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      return rows;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }
    rows[sqlite_required_text(*statement, 0)][sqlite_required_text(*statement, 1)] =
        sqlite3_column_double(statement.get(), 2);
  }
}

template <typename RecordMap, typename MatchFunction>
void compare_record_maps(ValidationReport& report,
                         const ValidationCodeRegistry& registry,
                         const RecordMap& package_records,
                         const RecordMap& index_records,
                         std::string_view entry,
                         std::string_view table,
                         std::string_view record_name,
                         MatchFunction values_match,
                         std::string_view code) {
  for (const auto& [id, package_record] : package_records) {
    const auto iterator = index_records.find(id);
    if (iterator == index_records.end()) {
      add_finding(report,
                  make_finding(registry, code, record_path(entry, package_record.line),
                               std::string{record_name} +
                                   " is missing from the SQLite index: " + id + "."));
      continue;
    }

    if (!values_match(package_record, iterator->second)) {
      add_finding(report,
                  make_finding(registry, code, record_path(entry, package_record.line),
                               std::string{record_name} +
                                   " does not match the SQLite index row: " + id + "."));
    }
  }

  for (const auto& [id, index_record] : index_records) {
    static_cast<void>(index_record);
    if (!package_records.contains(id)) {
      add_finding(report,
                  make_finding(registry, code, index_table_path(table),
                               "SQLite index row has no matching package record: " + id + "."));
    }
  }
}

void compare_text_fts(ValidationReport& report,
                      const ValidationCodeRegistry& registry,
                      const TextObservationMap& package_records,
                      const std::set<std::pair<std::string, std::string>>& index_rows) {
  for (const auto& [id, package_record] : package_records) {
    if (!index_rows.contains({id, package_record.normalized_text})) {
      add_text_index_issue(report, registry,
                           record_path("text/text_observations.jsonl",
                                       package_record.line),
                           "text_fts is missing the normalized text row for text observation: " +
                               id + ".");
    }
  }

  for (const auto& [id, text] : index_rows) {
    const auto iterator = package_records.find(id);
    if (iterator == package_records.end() ||
        iterator->second.normalized_text != text) {
      add_text_index_issue(report, registry, index_table_path("text_fts"),
                           "text_fts row has no matching package normalized text: " + id +
                               ".");
    }
  }
}

void compare_color_targets(
    ValidationReport& report,
    const ValidationCodeRegistry& registry,
    const ColorObservationMap& package_records,
    const std::set<std::string>& index_rows) {
  for (const auto& [id, package_record] : package_records) {
    const auto expected =
        id + "\n" + package_record.target_type + "\n" + package_record.target_id;
    if (!index_rows.contains(expected)) {
      add_color_index_issue(report, registry,
                            record_path("colors/color_observations.jsonl",
                                        package_record.line),
                            "color_targets is missing the package color observation target: " +
                                id + ".");
    }
  }

  for (const auto& row : index_rows) {
    std::istringstream parts{row};
    std::string id;
    std::getline(parts, id);
    if (!package_records.contains(id)) {
      add_color_index_issue(report, registry, index_table_path("color_targets"),
                            "color_targets row has no matching package color observation: " +
                                id + ".");
    }
  }
}

void compare_color_bucket_coverage(
    ValidationReport& report,
    const ValidationCodeRegistry& registry,
    const ColorObservationMap& package_records,
    const std::unordered_map<std::string, std::map<std::string, double>>& index_rows) {
  for (const auto& [id, package_record] : package_records) {
    const auto iterator = index_rows.find(id);
    if (iterator == index_rows.end()) {
      add_color_index_issue(report, registry,
                            record_path("colors/color_observations.jsonl",
                                        package_record.line),
                            "color_bucket_coverage is missing the package color observation: " +
                                id + ".");
      continue;
    }
    if (!coverage_values_match(package_record.bucket_coverage, iterator->second)) {
      add_color_index_issue(report, registry,
                            record_path("colors/color_observations.jsonl",
                                        package_record.line),
                            "color_bucket_coverage does not match package bucket coverage: " +
                                id + ".");
    }
  }

  for (const auto& [id, coverage] : index_rows) {
    static_cast<void>(coverage);
    if (!package_records.contains(id)) {
      add_color_index_issue(
          report, registry, index_table_path("color_bucket_coverage"),
          "color_bucket_coverage row has no matching package color observation: " + id +
              ".");
    }
  }
}

void validate_text_consistency(ValidationReport& report,
                               const ValidationCodeRegistry& registry,
                               const std::filesystem::path& package_path,
                               const svp::package::PackageLayout& layout,
                               sqlite3& database) {
  const auto package_regions = read_package_text_regions(package_path, layout);
  const auto package_observations = read_package_text_observations(package_path, layout);
  const auto package_numeric_values = read_package_numeric_values(package_path, layout);

  if (has_columns(report, registry, database, "text_regions",
                  {"text_region_id", "start_us", "end_us", "shot_id", "scene_id"})) {
    compare_record_maps(report, registry, package_regions, read_index_text_regions(database),
                        "text/text_regions.jsonl", "text_regions", "Text region",
                        text_region_values_match, kCodeTextIndexMismatch);
  }

  if (has_columns(report, registry, database, "text_observations",
                  {"text_observation_id", "text_region_id", "raw_text",
                   "normalized_text", "layout_class"})) {
    compare_record_maps(report, registry, package_observations,
                        read_index_text_observations(database),
                        "text/text_observations.jsonl", "text_observations",
                        "Text observation", text_observation_values_match,
                        kCodeTextIndexMismatch);
  }

  if (has_columns(report, registry, database, "numeric_values",
                  {"numeric_value_id", "text_observation_id", "text_region_id",
                   "numeric_value", "raw_text", "normalized_text"})) {
    compare_record_maps(report, registry, package_numeric_values,
                        read_index_numeric_values(database), "text/numeric_values.jsonl",
                        "numeric_values", "Numeric value", numeric_value_values_match,
                        kCodeTextIndexMismatch);
  }

  if (has_columns(report, registry, database, "text_fts",
                  {"object_id", "object_type", "text"})) {
    compare_text_fts(report, registry, package_observations, read_text_fts_rows(database));
  }
}

void validate_color_consistency(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                const std::filesystem::path& package_path,
                                const svp::package::PackageLayout& layout,
                                sqlite3& database) {
  const auto package_observations = read_package_color_observations(package_path, layout);

  if (has_columns(report, registry, database, "color_observations",
                  {"color_observation_id", "target_type", "target_id",
                   "dominant_bucket"})) {
    compare_record_maps(report, registry, package_observations,
                        read_index_color_observations(database),
                        "colors/color_observations.jsonl", "color_observations",
                        "Color observation", color_observation_values_match,
                        kCodeColorIndexMismatch);
  }

  if (has_columns(report, registry, database, "color_targets",
                  {"color_observation_id", "target_type", "target_id"})) {
    compare_color_targets(report, registry, package_observations,
                          read_color_target_rows(database));
  }

  if (has_columns(report, registry, database, "color_bucket_coverage",
                  {"color_observation_id", "bucket_id", "coverage"})) {
    compare_color_bucket_coverage(report, registry, package_observations,
                                  read_color_bucket_rows(database));
  }
}

}  // namespace

void add_ocr_color_index_consistency_findings(
    ValidationReport& report,
    const ValidationCodeRegistry& registry,
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout,
    sqlite3& database) {
  try {
    validate_text_consistency(report, registry, package_path, layout, database);
    validate_color_consistency(report, registry, package_path, layout, database);
  } catch (const std::exception& error) {
    add_finding(report, make_finding(registry, kCodeIndexSchemaInvalid,
                                     package_entry_path(kIndexSqliteEntry),
                                     error.what()));
  }
}

}  // namespace svp::validation
