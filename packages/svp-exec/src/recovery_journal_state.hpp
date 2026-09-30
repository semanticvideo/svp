#pragma once

// RecoveryJournal internals shared by the files that implement its methods
// (lifecycle, tasks, distributed records, resume, cleanup).

#include "file_lock.hpp"
#include "journal_manifest.hpp"
#include "sqlite_database.hpp"
#include "svp/exec/recovery_journal.hpp"

#include <string>

namespace svp::exec::detail {

struct JournalState {
  JournalLayout layout;
  JournalOptions options;
  JournalManifest manifest;
  // Declared before `database` so the database closes before the lock is
  // released.
  FileLock lock;
  SqliteDatabase database;

  [[nodiscard]] std::string now() const { return options.utc_now(); }
};

// Validates [A-Za-z0-9._-]+ (the TaskSpec task ID rule); throws
// JournalError(invalid_argument) naming `what`.
void require_record_identifier(std::string_view value, std::string_view what);

}  // namespace svp::exec::detail
