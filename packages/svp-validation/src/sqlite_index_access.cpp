#include "sqlite_index_access.hpp"

#include <zip.h>

#include <array>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace svp::validation {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

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

}  // namespace

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

}  // namespace svp::validation
