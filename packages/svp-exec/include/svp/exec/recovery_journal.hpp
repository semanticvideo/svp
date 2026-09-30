#pragma once

#include "svp/exec/journal_cleanup.hpp"
#include "svp/exec/journal_layout.hpp"
#include "svp/exec/journal_records.hpp"
#include "svp/exec/journal_resume.hpp"
#include "svp/exec/task_state.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace svp::exec {

namespace detail {
struct JournalState;
}

struct JournalOptions {
  // UTC timestamp source for journal-stamped columns (task.started_utc,
  // task.completed_utc, failure.occurred_utc, blob_location.recorded_utc).
  // Defaults to the system clock as "YYYY-MM-DDTHH:MM:SS.mmmZ".
  std::function<std::string()> utc_now;
};

// RC2 §20.4 recovery journal for one output path.
//
// The journal holds locks/build.lock exclusively for its whole lifetime, so a
// second create() or open() for the same output fails with
// JournalError(locked) instead of corrupting shared state. build.sqlite runs
// in WAL mode with synchronous=FULL. Artifacts are committed through
// blobs/pending (write, fsync, BLAKE3 verify) and an atomic rename into
// blobs/completed/<64 hex>, then recorded in one transaction with the task's
// `committed` state.
//
// Not thread-safe: one coordinator thread drives a journal.
class RecoveryJournal {
 public:
  // Creates `<output_path>-journal/` and records the session and sources.
  // Throws JournalError(already_exists) when the directory exists.
  static RecoveryJournal create(const std::filesystem::path& output_path,
                                const BuildSessionRecord& session,
                                std::span<const SourceFingerprintRecord> sources,
                                JournalOptions options = {});
  // Opens an existing journal for resume. Throws JournalError(not_found),
  // (locked), or (incompatible).
  static RecoveryJournal open(const std::filesystem::path& output_path,
                              JournalOptions options = {});

  RecoveryJournal(RecoveryJournal&&) noexcept;
  RecoveryJournal& operator=(RecoveryJournal&&) noexcept;
  RecoveryJournal(const RecoveryJournal&) = delete;
  RecoveryJournal& operator=(const RecoveryJournal&) = delete;
  ~RecoveryJournal();

  [[nodiscard]] const JournalLayout& layout() const;
  // The session the journal was created for (journal_manifest.json).
  [[nodiscard]] const std::string& build_session_id() const;

  void record_build_session(const BuildSessionRecord& session);
  void set_build_session_status(std::string_view session_id, BuildSessionStatus status);

  // Inserts the task as `planned` with its dependencies. Throws
  // (duplicate_record) when the task ID is already recorded.
  void record_task(const JournalTaskRecord& task);
  // Applies one allowed lifecycle edge (see task_state.hpp). Entering
  // `running` stamps started_utc. Throws (unknown_task) or
  // (invalid_transition); `committed` is only reachable via commit_task.
  void set_task_state(std::string_view task_id, TaskState next);
  [[nodiscard]] std::optional<TaskState> task_state(std::string_view task_id) const;

  // result_received -> committed. Every artifact is staged and verified
  // before any row changes; a mismatch throws (artifact_mismatch) and leaves
  // the task and journal untouched, so the caller records a failed attempt.
  void commit_task(const TaskCommit& commit);

  void record_failure(const std::optional<std::string>& task_id, std::string_view reason);

  void record_worker_session(const WorkerSessionRecord& session);
  void record_task_attempt(const TaskAttemptRecord& attempt);
  void record_blob_location(const BlobLocationRecord& location);

  // RC2 §20.4 `svp build --resume`: verifies `current_sources` against the
  // recorded fingerprints (throws source_mismatch), discards pending blobs,
  // re-hashes every completed artifact and demotes tasks whose outputs fail,
  // resets in-flight tasks, marks open attempts abandoned, and returns the
  // reconstructed DAG state.
  ResumeReport resume(std::span<const SourceFingerprintRecord> current_sources);

  // RC2 §20.5.1. Marks the journal's build session succeeded, then deletes
  // the whole journal directory (delete_on_success) or checkpoints and
  // closes it (retain_for_diagnostics). The journal is closed afterwards.
  JournalFinish finish_success(JournalRetention retention);

  // Checkpoints the WAL, closes the database, and releases the lock.
  void close();

 private:
  explicit RecoveryJournal(std::unique_ptr<detail::JournalState> state);
  [[nodiscard]] detail::JournalState& state() const;

  std::unique_ptr<detail::JournalState> state_;
};

}  // namespace svp::exec
