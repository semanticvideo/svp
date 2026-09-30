#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace svp::validation {

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

// A file removed when the owner goes out of scope.
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

// Copies one package entry (for example index/index.sqlite) to a unique
// temporary file so SQLite can open it.
[[nodiscard]] TemporaryFile extract_package_entry_to_temp_file(
    const std::filesystem::path& package_path,
    std::string_view entry);

// Opens a database read-only with extension loading disabled (RC2 Section
// 17.5 step 1).
[[nodiscard]] std::unique_ptr<sqlite3, SqliteDeleter> open_read_only_database(
    const std::filesystem::path& sqlite_path);

// User-defined tables and virtual tables of the main schema, excluding
// sqlite_* internal tables (RC2 Section 17.5 steps 2 and 3).
[[nodiscard]] std::set<std::string> read_user_table_names(sqlite3& database);

}  // namespace svp::validation
