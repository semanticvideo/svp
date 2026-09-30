#include "sqlite_database.hpp"

#include "svp/exec/journal_error.hpp"

#include <limits>
#include <sqlite3.h>

namespace svp::exec::detail {
namespace {

[[noreturn]] void throw_sqlite(sqlite3* database, int result, std::string_view context) {
  const std::string message = std::string(context) + ": " +
                              (database != nullptr ? sqlite3_errmsg(database) : sqlite3_errstr(result));
  // Extended result codes keep the primary code in the low byte (sqlite.org/rescode.html).
  constexpr int kPrimaryResultCodeMask = 0xFF;
  if ((result & kPrimaryResultCodeMask) == SQLITE_CONSTRAINT) {
    throw JournalError(JournalErrorCode::duplicate_record, message);
  }
  throw JournalError(JournalErrorCode::database_error, message);
}

}  // namespace

SqliteStatement::SqliteStatement(sqlite3* database, std::string_view sql) : database_(database) {
  const int result = sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()),
                                        &statement_, nullptr);
  if (result != SQLITE_OK) {
    throw_sqlite(database, result, "prepare");
  }
}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept
    : database_(other.database_), statement_(other.statement_) {
  other.statement_ = nullptr;
}

SqliteStatement::~SqliteStatement() {
  sqlite3_finalize(statement_);
}

SqliteStatement& SqliteStatement::bind(int index, std::string_view text) {
  const int result = sqlite3_bind_text(statement_, index, text.data(),
                                       static_cast<int>(text.size()), SQLITE_TRANSIENT);
  if (result != SQLITE_OK) {
    throw_sqlite(database_, result, "bind text");
  }
  return *this;
}

SqliteStatement& SqliteStatement::bind(int index, std::int64_t value) {
  const int result = sqlite3_bind_int64(statement_, index, value);
  if (result != SQLITE_OK) {
    throw_sqlite(database_, result, "bind integer");
  }
  return *this;
}

SqliteStatement& SqliteStatement::bind_unsigned(int index, std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw JournalError(JournalErrorCode::invalid_argument,
                       "value " + std::to_string(value) + " exceeds the SQLite INTEGER range");
  }
  return bind(index, static_cast<std::int64_t>(value));
}

SqliteStatement& SqliteStatement::bind_null(int index) {
  const int result = sqlite3_bind_null(statement_, index);
  if (result != SQLITE_OK) {
    throw_sqlite(database_, result, "bind null");
  }
  return *this;
}

SqliteStatement& SqliteStatement::bind(int index, const std::optional<std::string>& text) {
  return text ? bind(index, std::string_view(*text)) : bind_null(index);
}

SqliteStatement& SqliteStatement::bind(int index, const std::optional<std::int64_t>& value) {
  return value ? bind(index, *value) : bind_null(index);
}

SqliteStatement& SqliteStatement::bind_unsigned(int index,
                                                const std::optional<std::uint64_t>& value) {
  return value ? bind_unsigned(index, *value) : bind_null(index);
}

bool SqliteStatement::step() {
  const int result = sqlite3_step(statement_);
  if (result == SQLITE_ROW) {
    return true;
  }
  if (result == SQLITE_DONE) {
    return false;
  }
  throw_sqlite(database_, result, "step");
}

void SqliteStatement::run() {
  while (step()) {
  }
}

bool SqliteStatement::is_null(int column) const {
  return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

std::string SqliteStatement::text(int column) const {
  const auto* data = sqlite3_column_text(statement_, column);
  const int size = sqlite3_column_bytes(statement_, column);
  return data == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(data),
                                                       static_cast<std::size_t>(size));
}

std::int64_t SqliteStatement::integer(int column) const {
  return sqlite3_column_int64(statement_, column);
}

SqliteDatabase SqliteDatabase::open(const std::filesystem::path& path, OpenMode mode) {
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX;
  if (mode == OpenMode::create) {
    flags |= SQLITE_OPEN_CREATE;
  }
  sqlite3* database = nullptr;
  const int result = sqlite3_open_v2(path.c_str(), &database, flags, nullptr);
  if (result != SQLITE_OK) {
    const std::string message = "open " + path.string() + ": " +
                                (database != nullptr ? sqlite3_errmsg(database)
                                                     : sqlite3_errstr(result));
    sqlite3_close(database);
    throw JournalError(mode == OpenMode::existing ? JournalErrorCode::incompatible
                                                  : JournalErrorCode::database_error,
                       message);
  }
  sqlite3_extended_result_codes(database, 1);
  return SqliteDatabase(database);
}

SqliteDatabase::SqliteDatabase(SqliteDatabase&& other) noexcept : database_(other.database_) {
  other.database_ = nullptr;
}

SqliteDatabase& SqliteDatabase::operator=(SqliteDatabase&& other) noexcept {
  if (this != &other) {
    close();
    database_ = other.database_;
    other.database_ = nullptr;
  }
  return *this;
}

SqliteDatabase::~SqliteDatabase() {
  close();
}

void SqliteDatabase::close() noexcept {
  if (database_ != nullptr) {
    sqlite3_close_v2(database_);
    database_ = nullptr;
  }
}

void SqliteDatabase::exec(std::string_view sql) {
  char* error = nullptr;
  const std::string owned(sql);
  const int result = sqlite3_exec(database_, owned.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string message = error != nullptr ? error : sqlite3_errstr(result);
    sqlite3_free(error);
    throw_sqlite(nullptr, result, "exec: " + message);
  }
}

SqliteStatement SqliteDatabase::prepare(std::string_view sql) const {
  return SqliteStatement(database_, sql);
}

std::string SqliteDatabase::query_text(std::string_view sql) const {
  SqliteStatement statement = prepare(sql);
  return statement.step() ? statement.text(0) : std::string();
}

std::int64_t SqliteDatabase::query_integer(std::string_view sql) const {
  SqliteStatement statement = prepare(sql);
  return statement.step() ? statement.integer(0) : 0;
}

std::int64_t SqliteDatabase::changes() const noexcept {
  return sqlite3_changes64(database_);
}

SqliteTransaction::SqliteTransaction(SqliteDatabase& database) : database_(database) {
  database_.exec("BEGIN IMMEDIATE");
}

SqliteTransaction::~SqliteTransaction() {
  if (open_) {
    try {
      database_.exec("ROLLBACK");
    } catch (...) {
      // The connection is unusable; the next statement will report it.
    }
  }
}

void SqliteTransaction::commit() {
  database_.exec("COMMIT");
  open_ = false;
}

}  // namespace svp::exec::detail
