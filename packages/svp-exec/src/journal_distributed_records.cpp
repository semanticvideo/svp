// RecoveryJournal rows outside the task lifecycle: failures and the plan §4.6
// distributed-execution tables.

#include "recovery_journal_state.hpp"
#include "svp/exec/journal_error.hpp"

namespace svp::exec {

void RecoveryJournal::record_failure(const std::optional<std::string>& task_id,
                                     std::string_view reason) {
  detail::JournalState& journal = state();
  journal.database
      .prepare("INSERT INTO failure (task_id, occurred_utc, reason) VALUES (?1, ?2, ?3)")
      .bind(1, task_id)
      .bind(2, journal.now())
      .bind(3, reason)
      .run();
}

void RecoveryJournal::record_worker_session(const WorkerSessionRecord& session) {
  detail::require_record_identifier(session.worker_session_id, "worker session id");
  detail::SqliteStatement upsert = state().database.prepare(
      "INSERT OR REPLACE INTO worker_session "
      "(worker_session_id, worker_id, runtime_id, started_utc, ended_utc, status) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
  upsert.bind(1, session.worker_session_id).bind(2, session.worker_id);
  if (session.runtime_id) {
    upsert.bind(3, blake3_prefixed(*session.runtime_id));
  } else {
    upsert.bind_null(3);
  }
  upsert.bind(4, session.started_utc)
      .bind(5, session.ended_utc)
      .bind(6, worker_session_status_name(session.status))
      .run();
}

void RecoveryJournal::record_task_attempt(const TaskAttemptRecord& attempt) {
  detail::require_record_identifier(attempt.task_id, "task id");
  state()
      .database
      .prepare(
          "INSERT OR REPLACE INTO task_attempt "
          "(task_id, attempt, worker_session_id, lease_id, leased_utc, started_utc, "
          "completed_utc, queue_ms, transfer_ms, compute_ms, cpu_ms, peak_rss_bytes, outcome, "
          "error) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)")
      .bind(1, attempt.task_id)
      .bind(2, std::int64_t{attempt.attempt})
      .bind(3, attempt.worker_session_id)
      .bind(4, attempt.lease_id)
      .bind(5, attempt.leased_utc)
      .bind(6, attempt.started_utc)
      .bind(7, attempt.completed_utc)
      .bind_unsigned(8, attempt.queue_ms)
      .bind_unsigned(9, attempt.transfer_ms)
      .bind_unsigned(10, attempt.compute_ms)
      .bind_unsigned(11, attempt.cpu_ms)
      .bind_unsigned(12, attempt.peak_rss_bytes)
      .bind(13, attempt_outcome_name(attempt.outcome))
      .bind(14, attempt.error)
      .run();
}

void RecoveryJournal::record_blob_location(const BlobLocationRecord& location) {
  if (location.location.empty()) {
    throw JournalError(JournalErrorCode::invalid_argument, "blob location is empty");
  }
  detail::JournalState& journal = state();
  journal.database
      .prepare(
          "INSERT OR REPLACE INTO blob_location (blake3, location, byte_length, recorded_utc) "
          "VALUES (?1, ?2, ?3, ?4)")
      .bind(1, blake3_hex(location.blake3))
      .bind(2, location.location)
      .bind_unsigned(3, location.byte_length)
      .bind(4, journal.now())
      .run();
}

}  // namespace svp::exec
