#include "svp/exec/recovery_journal.hpp"

#include "journal_schema.hpp"
#include "record_identifiers.hpp"
#include "recovery_journal_state.hpp"
#include "svp/exec/journal_error.hpp"
#include "utc_clock.hpp"

#include <unistd.h>

namespace svp::exec {
namespace {

namespace fs = std::filesystem;

[[noreturn]] void io_failure(std::string_view what, const std::error_code& error) {
  throw JournalError(JournalErrorCode::io_error, std::string(what) + ": " + error.message());
}

JournalOptions with_default_clock(JournalOptions options) {
  if (!options.utc_now) {
    options.utc_now = detail::utc_now_millis;
  }
  return options;
}

detail::FileLock acquire_build_lock(const JournalLayout& layout) {
  detail::LockAttempt attempt =
      detail::lock_file(layout.lock_file, detail::LockMode::exclusive, detail::LockWait::try_once);
  if (attempt.outcome == detail::LockOutcome::contended) {
    const std::string holder = detail::read_lock_note(layout.lock_file);
    throw JournalError(JournalErrorCode::locked,
                       "recovery journal " + layout.root.string() +
                           " is in use by another build" +
                           (holder.empty() ? std::string() : " (" + holder + ")"));
  }
  if (attempt.outcome == detail::LockOutcome::failed) {
    io_failure("lock " + layout.lock_file.string(), attempt.error);
  }
  // Diagnostic only; failure to write it does not weaken the lock.
  (void)attempt.lock.write_note("pid " + std::to_string(::getpid()));
  return std::move(attempt.lock);
}

void insert_build_session(detail::SqliteDatabase& database, const BuildSessionRecord& session) {
  detail::require_record_identifier(session.id, "build session id");
  database
      .prepare(
          "INSERT INTO build_session (id, started_utc, svp_version, builder_version, status) "
          "VALUES (?1, ?2, ?3, ?4, ?5)")
      .bind(1, session.id)
      .bind(2, session.started_utc)
      .bind(3, session.svp_version)
      .bind(4, session.builder_version)
      .bind(5, build_session_status_name(session.status))
      .run();
}

void insert_source(detail::SqliteDatabase& database, const SourceFingerprintRecord& source) {
  detail::require_record_identifier(source.source_id, "source id");
  database
      .prepare(
          "INSERT INTO source_fingerprint (source_id, path, size_bytes, mtime_ns, blake3) "
          "VALUES (?1, ?2, ?3, ?4, ?5)")
      .bind(1, source.source_id)
      .bind(2, source.path)
      .bind_unsigned(3, source.size_bytes)
      .bind(4, source.mtime_ns)
      .bind(5, blake3_hex(source.blake3))
      .run();
}

void initialize_new_journal(detail::JournalState& impl, const fs::path& output_path,
                            const BuildSessionRecord& session,
                            std::span<const SourceFingerprintRecord> sources) {
  const JournalLayout& layout = impl.layout;
  std::error_code error;
  for (const fs::path& directory : {layout.locks, layout.pending, layout.completed}) {
    fs::create_directories(directory, error);
    if (error) {
      io_failure("create " + directory.string(), error);
    }
  }
  impl.lock = acquire_build_lock(layout);

  impl.database =
      detail::SqliteDatabase::open(layout.database, detail::SqliteDatabase::OpenMode::create);
  detail::enable_journal_pragmas(impl.database);
  detail::apply_journal_schema(impl.database);
  {
    detail::SqliteTransaction transaction(impl.database);
    insert_build_session(impl.database, session);
    for (const SourceFingerprintRecord& source : sources) {
      insert_source(impl.database, source);
    }
    transaction.commit();
  }

  impl.manifest = detail::JournalManifest{
      .build_session_id = session.id,
      .created_utc = impl.now(),
      .output_path = fs::absolute(output_path, error).string(),
      .journal_schema_version = detail::kJournalSchemaVersion};
  // Written last: its presence marks a fully initialized journal.
  detail::write_journal_manifest(layout, impl.manifest);
}

}  // namespace

namespace detail {

void require_record_identifier(std::string_view value, std::string_view what) {
  if (!is_record_identifier(value)) {
    throw JournalError(JournalErrorCode::invalid_argument,
                       std::string(what) + " must be [A-Za-z0-9._-]+: \"" + std::string(value) +
                           "\"");
  }
}

}  // namespace detail

RecoveryJournal::RecoveryJournal(std::unique_ptr<detail::JournalState> state)
    : state_(std::move(state)) {}
RecoveryJournal::RecoveryJournal(RecoveryJournal&&) noexcept = default;
RecoveryJournal& RecoveryJournal::operator=(RecoveryJournal&&) noexcept = default;

RecoveryJournal::~RecoveryJournal() {
  close();
}

detail::JournalState& RecoveryJournal::state() const {
  if (!state_ || !state_->database.is_open()) {
    throw JournalError(JournalErrorCode::closed, "recovery journal is closed");
  }
  return *state_;
}

RecoveryJournal RecoveryJournal::create(const fs::path& output_path,
                                        const BuildSessionRecord& session,
                                        std::span<const SourceFingerprintRecord> sources,
                                        JournalOptions options) {
  auto impl = std::make_unique<detail::JournalState>();
  impl->layout = journal_layout_for(output_path);
  impl->options = with_default_clock(std::move(options));
  const JournalLayout& layout = impl->layout;

  std::error_code error;
  // Non-recursive and atomic: exactly one creator wins the directory.
  if (!fs::create_directory(layout.root, error)) {
    if (error) {
      io_failure("create " + layout.root.string(), error);
    }
    throw JournalError(JournalErrorCode::already_exists,
                       "recovery journal " + layout.root.string() +
                           " already exists; resume it or remove it");
  }
  try {
    initialize_new_journal(*impl, output_path, session, sources);
  } catch (...) {
    // This call created the directory, so a half-initialized journal is
    // removed rather than left for a later open() to reject.
    impl->database.close();
    impl->lock.release();
    std::error_code ignored;
    fs::remove_all(layout.root, ignored);
    throw;
  }
  return RecoveryJournal(std::move(impl));
}

RecoveryJournal RecoveryJournal::open(const fs::path& output_path, JournalOptions options) {
  auto impl = std::make_unique<detail::JournalState>();
  impl->layout = journal_layout_for(output_path);
  impl->options = with_default_clock(std::move(options));
  const JournalLayout& layout = impl->layout;

  std::error_code error;
  if (!fs::is_directory(layout.root, error)) {
    throw JournalError(JournalErrorCode::not_found,
                       "no recovery journal at " + layout.root.string());
  }
  fs::create_directories(layout.locks, error);
  if (error) {
    io_failure("create " + layout.locks.string(), error);
  }
  impl->lock = acquire_build_lock(layout);
  impl->manifest = detail::read_journal_manifest(layout);
  for (const fs::path& directory : {layout.pending, layout.completed}) {
    fs::create_directories(directory, error);
    if (error) {
      io_failure("create " + directory.string(), error);
    }
  }
  impl->database = detail::SqliteDatabase::open(layout.database,
                                                detail::SqliteDatabase::OpenMode::existing);
  detail::check_journal_schema(impl->database);
  detail::enable_journal_pragmas(impl->database);
  return RecoveryJournal(std::move(impl));
}

const JournalLayout& RecoveryJournal::layout() const {
  return state().layout;
}

const std::string& RecoveryJournal::build_session_id() const {
  return state().manifest.build_session_id;
}

void RecoveryJournal::record_build_session(const BuildSessionRecord& session) {
  insert_build_session(state().database, session);
}

void RecoveryJournal::set_build_session_status(std::string_view session_id,
                                               BuildSessionStatus status) {
  detail::SqliteDatabase& database = state().database;
  database.prepare("UPDATE build_session SET status = ?1 WHERE id = ?2")
      .bind(1, build_session_status_name(status))
      .bind(2, session_id)
      .run();
  if (database.changes() == 0) {
    throw JournalError(JournalErrorCode::invalid_argument,
                       "unknown build session \"" + std::string(session_id) + "\"");
  }
}

void RecoveryJournal::close() {
  if (!state_) {
    return;
  }
  if (state_->database.is_open()) {
    try {
      // Fold the WAL back into build.sqlite so a retained journal is one file.
      state_->database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    } catch (const JournalError&) {
      // The WAL stays valid; SQLite replays it on the next open.
    }
    state_->database.close();
  }
  state_->lock.release();
}

}  // namespace svp::exec
