#include "journal_schema.hpp"

#include "svp/exec/journal_error.hpp"

#include <string>

namespace svp::exec::detail {
namespace {

// RC2 §20.4 "Minimum required journal tables", verbatim.
constexpr std::string_view kMinimumTablesSql = R"sql(
CREATE TABLE build_session (
  id TEXT PRIMARY KEY,
  started_utc TEXT NOT NULL,
  svp_version TEXT NOT NULL,
  builder_version TEXT NOT NULL,
  status TEXT NOT NULL
);

CREATE TABLE source_fingerprint (
  source_id TEXT PRIMARY KEY,
  path TEXT NOT NULL,
  size_bytes INTEGER NOT NULL,
  mtime_ns INTEGER,
  blake3 TEXT NOT NULL
);

CREATE TABLE task (
  task_id TEXT PRIMARY KEY,
  task_type TEXT NOT NULL,
  status TEXT NOT NULL,
  cache_key TEXT,
  started_utc TEXT,
  completed_utc TEXT,
  output_blake3 TEXT
);

CREATE TABLE task_dependency (
  task_id TEXT NOT NULL,
  depends_on_task_id TEXT NOT NULL,
  PRIMARY KEY (task_id, depends_on_task_id)
);

CREATE TABLE artifact (
  artifact_id TEXT PRIMARY KEY,
  task_id TEXT NOT NULL,
  relative_path TEXT NOT NULL,
  blake3 TEXT NOT NULL,
  byte_length INTEGER NOT NULL
);

CREATE TABLE artifact_provenance (
  artifact_id TEXT PRIMARY KEY,
  processor_id TEXT NOT NULL,
  parameters_blake3 TEXT NOT NULL,
  model_refs_json TEXT NOT NULL
);

CREATE TABLE cache_hit (
  task_id TEXT PRIMARY KEY,
  cache_key TEXT NOT NULL,
  artifact_id TEXT NOT NULL,
  verified INTEGER NOT NULL
);

CREATE TABLE failure (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT,
  occurred_utc TEXT NOT NULL,
  reason TEXT NOT NULL
);
)sql";

// Plan §4.6 distributed-execution tables. Non-normative: RC2 does not define
// them and the package never contains them.
constexpr std::string_view kDistributedTablesSql = R"sql(
CREATE TABLE worker_session (
  worker_session_id TEXT PRIMARY KEY,
  worker_id TEXT NOT NULL,
  runtime_id TEXT,
  started_utc TEXT NOT NULL,
  ended_utc TEXT,
  status TEXT NOT NULL
);

CREATE TABLE task_attempt (
  task_id TEXT NOT NULL,
  attempt INTEGER NOT NULL,
  worker_session_id TEXT,
  lease_id TEXT,
  leased_utc TEXT,
  started_utc TEXT,
  completed_utc TEXT,
  queue_ms INTEGER,
  transfer_ms INTEGER,
  compute_ms INTEGER,
  cpu_ms INTEGER,
  peak_rss_bytes INTEGER,
  outcome TEXT NOT NULL,
  error TEXT,
  PRIMARY KEY (task_id, attempt)
);

CREATE TABLE blob_location (
  blake3 TEXT NOT NULL,
  location TEXT NOT NULL,
  byte_length INTEGER NOT NULL,
  recorded_utc TEXT NOT NULL,
  PRIMARY KEY (blake3, location)
);
)sql";

// Lookup indexes. They add no columns or constraints to the tables above.
constexpr std::string_view kIndexesSql = R"sql(
-- commit and resume look up artifacts by task.
CREATE INDEX artifact_by_task ON artifact (task_id);
)sql";

}  // namespace

void apply_journal_schema(SqliteDatabase& database) {
  SqliteTransaction transaction(database);
  database.exec(kMinimumTablesSql);
  database.exec(kDistributedTablesSql);
  database.exec(kIndexesSql);
  database.exec("PRAGMA user_version = " + std::to_string(kJournalSchemaVersion));
  transaction.commit();
}

void check_journal_schema(const SqliteDatabase& database) {
  const std::int64_t version = database.query_integer("PRAGMA user_version");
  if (version != kJournalSchemaVersion) {
    throw JournalError(JournalErrorCode::incompatible,
                       "journal schema version " + std::to_string(version) + ", expected " +
                           std::to_string(kJournalSchemaVersion));
  }
}

void enable_journal_pragmas(SqliteDatabase& database) {
  const std::string mode = database.query_text("PRAGMA journal_mode = WAL");
  if (mode != "wal") {
    throw JournalError(JournalErrorCode::database_error,
                       "build.sqlite could not enter WAL mode (journal_mode=" + mode + ")");
  }
  database.exec("PRAGMA synchronous = FULL");
}

}  // namespace svp::exec::detail
