#include "index_validation.hpp"

#include "index_logical_rows.hpp"
#include "json_schema_subset.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <zip.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
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

struct ZipDeleter {
  void operator()(zip_t* archive) const noexcept {
    if (archive != nullptr) {
      zip_discard(archive);
    }
  }
};

struct ZipFileDeleter {
  void operator()(zip_file_t* file) const noexcept {
    if (file != nullptr) {
      zip_fclose(file);
    }
  }
};

struct SqliteDeleter {
  void operator()(sqlite3* database) const noexcept {
    if (database != nullptr) {
      sqlite3_close(database);
    }
  }
};

struct StatementDeleter {
  void operator()(sqlite3_stmt* statement) const noexcept {
    if (statement != nullptr) {
      sqlite3_finalize(statement);
    }
  }
};

class TemporaryFile {
 public:
  explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  TemporaryFile(TemporaryFile&& other) noexcept : path_(std::move(other.path_)) {}

  TemporaryFile& operator=(TemporaryFile&& other) noexcept {
    if (this != &other) {
      cleanup();
      path_ = std::move(other.path_);
    }
    return *this;
  }

  ~TemporaryFile() {
    cleanup();
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  void cleanup() noexcept {
    if (path_.empty()) {
      return;
    }

    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    path_.clear();
  }

  std::filesystem::path path_;
};

std::string package_entry_path(std::string_view entry) {
  return "/" + std::string{entry};
}

std::string zip_error_message(int error_code) {
  zip_error_t error;
  zip_error_init_with_code(&error, error_code);
  std::string message = zip_error_strerror(&error);
  zip_error_fini(&error);
  return message;
}

std::filesystem::path unique_temp_sqlite_path() {
  const auto base = std::filesystem::temp_directory_path();
  std::random_device random;
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();

  for (int attempt = 0; attempt < 32; ++attempt) {
    std::ostringstream name;
    name << "svp-validator-index-" << ticks << "-" << random() << "-" << attempt
         << ".sqlite";
    auto path = base / name.str();
    if (!std::filesystem::exists(path)) {
      return path;
    }
  }

  throw std::runtime_error("could not allocate a temporary SQLite path");
}

TemporaryFile extract_package_entry_to_temp_file(const std::filesystem::path& package_path,
                                                 std::string_view entry) {
  int error_code = ZIP_ER_OK;
  std::unique_ptr<zip_t, ZipDeleter> archive{
      zip_open(package_path.string().c_str(), ZIP_RDONLY, &error_code)};
  if (!archive) {
    throw std::runtime_error(zip_error_message(error_code));
  }

  std::unique_ptr<zip_file_t, ZipFileDeleter> file{
      zip_fopen(archive.get(), std::string{entry}.c_str(), 0)};
  if (!file) {
    throw std::runtime_error(zip_strerror(archive.get()));
  }

  TemporaryFile output_path{unique_temp_sqlite_path()};
  std::ofstream output{output_path.path(), std::ios::binary};
  if (!output) {
    throw std::runtime_error("could not create temporary SQLite file");
  }

  std::array<char, 64 * 1024> buffer{};
  while (true) {
    const auto bytes_read = zip_fread(file.get(), buffer.data(), buffer.size());
    if (bytes_read < 0) {
      throw std::runtime_error(zip_file_strerror(file.get()));
    }
    if (bytes_read == 0) {
      break;
    }

    output.write(buffer.data(), bytes_read);
    if (!output) {
      throw std::runtime_error("could not write temporary SQLite file");
    }
  }

  return output_path;
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

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
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

std::unique_ptr<sqlite3, SqliteDeleter> open_read_only_database(
    const std::filesystem::path& sqlite_path) {
  sqlite3* raw_database = nullptr;
  const auto status = sqlite3_open_v2(sqlite_path.string().c_str(), &raw_database,
                                      SQLITE_OPEN_READONLY, nullptr);
  std::unique_ptr<sqlite3, SqliteDeleter> database{raw_database};
  if (status != SQLITE_OK) {
    const std::string message =
        raw_database == nullptr ? "could not open SQLite database" : sqlite3_errmsg(raw_database);
    throw std::runtime_error(message);
  }

  sqlite3_db_config(database.get(), SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
  return database;
}

std::set<std::string> read_user_table_names(sqlite3& database) {
  constexpr std::string_view table_list_query = "PRAGMA table_list";

  sqlite3_stmt* raw_table_list_statement = nullptr;
  if (sqlite3_prepare_v2(&database, table_list_query.data(),
                         static_cast<int>(table_list_query.size()),
                         &raw_table_list_statement, nullptr) == SQLITE_OK) {
    std::unique_ptr<sqlite3_stmt, StatementDeleter> statement{raw_table_list_statement};
    std::set<std::string> table_names;
    while (true) {
      const auto step = sqlite3_step(statement.get());
      if (step == SQLITE_DONE) {
        return table_names;
      }
      if (step != SQLITE_ROW) {
        break;
      }

      const auto* schema_text = sqlite3_column_text(statement.get(), 0);
      const auto* name_text = sqlite3_column_text(statement.get(), 1);
      const auto* type_text = sqlite3_column_text(statement.get(), 2);
      if (schema_text == nullptr || name_text == nullptr || type_text == nullptr) {
        continue;
      }

      const std::string schema = reinterpret_cast<const char*>(schema_text);
      const std::string name = reinterpret_cast<const char*>(name_text);
      const std::string type = reinterpret_cast<const char*>(type_text);
      if (schema == "main" && (type == "table" || type == "virtual") &&
          !starts_with(name, "sqlite_")) {
        table_names.insert(name);
      }
    }
  } else if (raw_table_list_statement != nullptr) {
    sqlite3_finalize(raw_table_list_statement);
  }

  constexpr std::string_view query =
      "SELECT name FROM sqlite_schema "
      "WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
      "ORDER BY name";

  sqlite3_stmt* raw_statement = nullptr;
  if (sqlite3_prepare_v2(&database, query.data(), static_cast<int>(query.size()),
                         &raw_statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(&database));
  }

  std::unique_ptr<sqlite3_stmt, StatementDeleter> statement{raw_statement};
  std::set<std::string> table_names;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }

    const auto* text = sqlite3_column_text(statement.get(), 0);
    if (text != nullptr) {
      table_names.insert(reinterpret_cast<const char*>(text));
    }
  }

  return table_names;
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
                          const LogicalRowStreamSummary& stream) {
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
                        const LogicalRowStreamSummary& stream) {
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
                                  const LogicalRowStreamSummary& stream) {
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

std::optional<LogicalRowStreamSummary> validate_sqlite_index(
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
    return compute_logical_row_stream_summary(*database, table_names);
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
