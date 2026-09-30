#pragma once

// Minimal RAII over the sqlite3 C API for the recovery journal. Every failure
// throws JournalError: database_error, or duplicate_record for constraint
// violations.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace svp::exec::detail {

class SqliteStatement {
 public:
  SqliteStatement(sqlite3* database, std::string_view sql);
  SqliteStatement(SqliteStatement&&) noexcept;
  SqliteStatement& operator=(SqliteStatement&&) = delete;
  SqliteStatement(const SqliteStatement&) = delete;
  SqliteStatement& operator=(const SqliteStatement&) = delete;
  ~SqliteStatement();

  // Parameters are 1-based, as in SQLite.
  SqliteStatement& bind(int index, std::string_view text);
  // Exact-match overloads so std::string and literals do not also convert to
  // the std::optional<std::string> overload.
  SqliteStatement& bind(int index, const std::string& text) {
    return bind(index, std::string_view(text));
  }
  SqliteStatement& bind(int index, const char* text) { return bind(index, std::string_view(text)); }
  SqliteStatement& bind(int index, std::int64_t value);
  // Throws invalid_argument above INT64_MAX (SQLite INTEGER is signed).
  SqliteStatement& bind_unsigned(int index, std::uint64_t value);
  SqliteStatement& bind_null(int index);
  SqliteStatement& bind(int index, const std::optional<std::string>& text);
  SqliteStatement& bind(int index, const std::optional<std::int64_t>& value);
  SqliteStatement& bind_unsigned(int index, const std::optional<std::uint64_t>& value);

  // True when a row is available, false when the statement is done.
  bool step();
  // Steps a statement that returns no rows.
  void run();

  // Columns are 0-based, as in SQLite.
  [[nodiscard]] bool is_null(int column) const;
  [[nodiscard]] std::string text(int column) const;
  [[nodiscard]] std::int64_t integer(int column) const;

 private:
  sqlite3* database_ = nullptr;
  sqlite3_stmt* statement_ = nullptr;
};

class SqliteDatabase {
 public:
  enum class OpenMode { create, existing };

  SqliteDatabase() = default;
  static SqliteDatabase open(const std::filesystem::path& path, OpenMode mode);
  SqliteDatabase(SqliteDatabase&&) noexcept;
  SqliteDatabase& operator=(SqliteDatabase&&) noexcept;
  SqliteDatabase(const SqliteDatabase&) = delete;
  SqliteDatabase& operator=(const SqliteDatabase&) = delete;
  ~SqliteDatabase();

  [[nodiscard]] bool is_open() const noexcept { return database_ != nullptr; }
  void close() noexcept;

  void exec(std::string_view sql);
  [[nodiscard]] SqliteStatement prepare(std::string_view sql) const;
  // First column of the first row of a single-value query ("PRAGMA ...").
  [[nodiscard]] std::string query_text(std::string_view sql) const;
  [[nodiscard]] std::int64_t query_integer(std::string_view sql) const;
  // Rows changed by the most recent INSERT, UPDATE, or DELETE.
  [[nodiscard]] std::int64_t changes() const noexcept;

 private:
  explicit SqliteDatabase(sqlite3* database) noexcept : database_(database) {}
  sqlite3* database_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back when destroyed uncommitted.
class SqliteTransaction {
 public:
  explicit SqliteTransaction(SqliteDatabase& database);
  SqliteTransaction(const SqliteTransaction&) = delete;
  SqliteTransaction& operator=(const SqliteTransaction&) = delete;
  ~SqliteTransaction();

  void commit();

 private:
  SqliteDatabase& database_;
  bool open_ = true;
};

}  // namespace svp::exec::detail
