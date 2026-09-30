#pragma once

#include "sqlite_database.hpp"

#include <string_view>

namespace svp::exec::detail {

// Stored in PRAGMA user_version. Bump when a table or column changes; open()
// refuses journals written with another version.
inline constexpr std::int64_t kJournalSchemaVersion = 1;

// Creates every table (RC2 §20.4 minimum set plus the plan §4.6
// distributed-execution tables) and stamps kJournalSchemaVersion.
void apply_journal_schema(SqliteDatabase& database);

// Throws JournalError(incompatible) unless user_version matches.
void check_journal_schema(const SqliteDatabase& database);

// WAL while the build is active (RC2 §20.4) and synchronous=FULL, so every
// committed transaction survives power loss, not just a process crash.
void enable_journal_pragmas(SqliteDatabase& database);

}  // namespace svp::exec::detail
