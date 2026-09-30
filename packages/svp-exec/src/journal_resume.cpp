// RecoveryJournal::resume (RC2 §20.4 `svp build --resume`).

#include "recovery_journal_state.hpp"
#include "svp/exec/journal_error.hpp"
#include "verified_blob.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace svp::exec {
namespace {

namespace fs = std::filesystem;

struct RecordedSource {
  std::uint64_t size_bytes = 0;
  std::string blake3;
};

// Source identity is (source_id, size_bytes, blake3). Path and mtime may
// legitimately change when the user moves or touches the file.
void verify_sources(const detail::SqliteDatabase& database,
                    std::span<const SourceFingerprintRecord> current_sources) {
  std::map<std::string, RecordedSource> recorded;
  detail::SqliteStatement query =
      database.prepare("SELECT source_id, size_bytes, blake3 FROM source_fingerprint");
  while (query.step()) {
    recorded.emplace(query.text(0),
                     RecordedSource{.size_bytes = static_cast<std::uint64_t>(query.integer(1)),
                                    .blake3 = query.text(2)});
  }
  std::set<std::string> seen;
  for (const SourceFingerprintRecord& source : current_sources) {
    const auto found = recorded.find(source.source_id);
    if (found == recorded.end()) {
      throw JournalError(JournalErrorCode::source_mismatch,
                         "source " + source.source_id + " is not part of the journaled build");
    }
    if (found->second.size_bytes != source.size_bytes ||
        found->second.blake3 != blake3_hex(source.blake3)) {
      throw JournalError(JournalErrorCode::source_mismatch,
                         "source " + source.source_id + " (" + source.path +
                             ") changed since the journal was written");
    }
    seen.insert(source.source_id);
  }
  for (const auto& [source_id, unused] : recorded) {
    if (!seen.contains(source_id)) {
      throw JournalError(JournalErrorCode::source_mismatch,
                         "journaled source " + source_id + " was not supplied for resume");
    }
  }
}

// Nothing in pending/ was ever verified and renamed, so none of it is trusted.
std::uint64_t discard_pending(const fs::path& pending) {
  std::uint64_t removed = 0;
  std::error_code error;
  for (fs::directory_iterator entry(pending, error), end; !error && entry != end;
       entry.increment(error)) {
    std::error_code remove_error;
    removed += fs::remove_all(entry->path(), remove_error) > 0 ? 1 : 0;
  }
  if (error) {
    throw JournalError(JournalErrorCode::io_error,
                       "clear " + pending.string() + ": " + error.message());
  }
  return removed;
}

std::optional<DemotionReason> demotion_for(detail::BlobCheck check) {
  switch (check) {
    case detail::BlobCheck::valid:
      return std::nullopt;
    case detail::BlobCheck::missing:
      return DemotionReason::artifact_missing;
    case detail::BlobCheck::not_regular_file:
      return DemotionReason::artifact_not_regular_file;
    case detail::BlobCheck::size_mismatch:
      return DemotionReason::artifact_size_mismatch;
    case detail::BlobCheck::digest_mismatch:
      return DemotionReason::artifact_hash_mismatch;
    case detail::BlobCheck::io_error:
      break;
  }
  return DemotionReason::artifact_missing;
}

class ArtifactVerifier {
 public:
  explicit ArtifactVerifier(const JournalLayout& layout) : layout_(layout) {}

  // Re-hashes each completed blob once, however many artifacts share it.
  std::optional<DemotionReason> check(const std::string& relative_path,
                                      const std::string& blake3_column, std::int64_t byte_length) {
    const auto digest = parse_blake3_hex(blake3_column);
    if (!digest || byte_length < 0 ||
        relative_path != journal_completed_relative_path(blake3_column)) {
      return DemotionReason::artifact_record_invalid;
    }
    const auto cached = results_.find(relative_path);
    if (cached != results_.end()) {
      return cached->second;
    }
    std::error_code error;
    const detail::BlobCheck result =
        detail::check_blob(layout_.completed / blake3_column, *digest,
                           static_cast<std::uint64_t>(byte_length), error);
    if (result == detail::BlobCheck::io_error) {
      throw JournalError(JournalErrorCode::io_error,
                         "verify " + relative_path + ": " + error.message());
    }
    return results_[relative_path] = demotion_for(result);
  }

 private:
  const JournalLayout& layout_;
  std::map<std::string, std::optional<DemotionReason>> results_;
};

void demote_task(detail::SqliteDatabase& database, const std::string& task_id) {
  database
      .prepare(
          "DELETE FROM artifact_provenance WHERE artifact_id IN "
          "(SELECT artifact_id FROM artifact WHERE task_id = ?1)")
      .bind(1, task_id)
      .run();
  database.prepare("DELETE FROM artifact WHERE task_id = ?1").bind(1, task_id).run();
  database.prepare("DELETE FROM cache_hit WHERE task_id = ?1").bind(1, task_id).run();
  database
      .prepare(
          "UPDATE task SET status = ?1, output_blake3 = NULL, completed_utc = NULL "
          "WHERE task_id = ?2")
      .bind(1, task_state_name(TaskState::planned))
      .bind(2, task_id)
      .run();
}

std::vector<DemotedTask> verify_committed_tasks(detail::SqliteDatabase& database,
                                                const JournalLayout& layout) {
  std::vector<std::pair<std::string, bool>> committed;  // task_id, has output
  {
    detail::SqliteStatement query = database.prepare(
        "SELECT task_id, output_blake3 FROM task WHERE status = ?1 ORDER BY task_id");
    query.bind(1, task_state_name(TaskState::committed));
    while (query.step()) {
      committed.emplace_back(query.text(0), !query.is_null(1));
    }
  }
  ArtifactVerifier verifier(layout);
  std::vector<DemotedTask> demoted;
  for (const auto& [task_id, has_output] : committed) {
    std::optional<DemotedTask> failure;
    if (!has_output) {
      failure = DemotedTask{.task_id = task_id, .reason = DemotionReason::output_digest_missing};
    } else {
      detail::SqliteStatement artifacts = database.prepare(
          "SELECT artifact_id, relative_path, blake3, byte_length FROM artifact "
          "WHERE task_id = ?1 ORDER BY artifact_id");
      artifacts.bind(1, task_id);
      while (!failure && artifacts.step()) {
        if (const auto reason =
                verifier.check(artifacts.text(1), artifacts.text(2), artifacts.integer(3))) {
          failure =
              DemotedTask{.task_id = task_id, .artifact_id = artifacts.text(0), .reason = *reason};
        }
      }
    }
    if (failure) {
      demote_task(database, task_id);
      demoted.push_back(std::move(*failure));
    }
  }
  return demoted;
}

// Removes completed blobs that no artifact row references: a crash between
// rename and commit, or content that failed verification above.
std::uint64_t discard_unreferenced_completed(const detail::SqliteDatabase& database,
                                             const JournalLayout& layout) {
  std::set<std::string> referenced;
  detail::SqliteStatement query = database.prepare("SELECT DISTINCT relative_path FROM artifact");
  while (query.step()) {
    referenced.insert(query.text(0));
  }
  std::uint64_t removed = 0;
  std::error_code error;
  for (fs::directory_iterator entry(layout.completed, error), end; !error && entry != end;
       entry.increment(error)) {
    const std::string relative =
        journal_completed_relative_path(entry->path().filename().string());
    if (referenced.contains(relative)) {
      continue;
    }
    std::error_code remove_error;
    removed += fs::remove_all(entry->path(), remove_error) > 0 ? 1 : 0;
  }
  if (error) {
    throw JournalError(JournalErrorCode::io_error,
                       "scan " + layout.completed.string() + ": " + error.message());
  }
  return removed;
}

// Every task that is not committed restarts from `ready` (all dependencies
// committed) or `planned`. Nothing that was in flight is trusted.
std::vector<ResumedTask> reset_incomplete_tasks(detail::SqliteDatabase& database) {
  std::map<std::string, ResumedTask> tasks;
  {
    detail::SqliteStatement query =
        database.prepare("SELECT task_id, task_type, status FROM task");
    while (query.step()) {
      const auto state = parse_task_state(query.text(2));
      tasks.emplace(query.text(0),
                    ResumedTask{.task_id = query.text(0),
                                .task_type = query.text(1),
                                .state = state.value_or(TaskState::planned)});
    }
  }
  {
    detail::SqliteStatement query = database.prepare(
        "SELECT task_id, depends_on_task_id FROM task_dependency "
        "ORDER BY task_id, depends_on_task_id");
    while (query.step()) {
      const auto task = tasks.find(query.text(0));
      if (task != tasks.end()) {
        task->second.depends_on.push_back(query.text(1));
      }
    }
  }
  const auto is_committed = [&](const std::string& task_id) {
    const auto found = tasks.find(task_id);
    return found != tasks.end() && found->second.state == TaskState::committed;
  };
  std::vector<ResumedTask> resumed;
  resumed.reserve(tasks.size());
  for (auto& [task_id, task] : tasks) {
    if (task.state != TaskState::committed) {
      const bool ready = std::all_of(task.depends_on.begin(), task.depends_on.end(), is_committed);
      task.state = ready ? TaskState::ready : TaskState::planned;
      database.prepare("UPDATE task SET status = ?1 WHERE task_id = ?2")
          .bind(1, task_state_name(task.state))
          .bind(2, task_id)
          .run();
    }
    resumed.push_back(task);
  }
  return resumed;
}

std::uint64_t abandon_open_attempts(detail::SqliteDatabase& database) {
  database
      .prepare("UPDATE task_attempt SET outcome = ?1 WHERE outcome IN (?2, ?3)")
      .bind(1, attempt_outcome_name(AttemptOutcome::abandoned))
      .bind(2, attempt_outcome_name(AttemptOutcome::leased))
      .bind(3, attempt_outcome_name(AttemptOutcome::running))
      .run();
  const auto abandoned = static_cast<std::uint64_t>(database.changes());
  database
      .prepare("UPDATE worker_session SET status = ?1 WHERE status = ?2")
      .bind(1, worker_session_status_name(WorkerSessionStatus::lost))
      .bind(2, worker_session_status_name(WorkerSessionStatus::active))
      .run();
  return abandoned;
}

}  // namespace

ResumeReport RecoveryJournal::resume(std::span<const SourceFingerprintRecord> current_sources) {
  detail::JournalState& journal = state();
  verify_sources(journal.database, current_sources);

  ResumeReport report;
  report.discarded_pending_blobs = discard_pending(journal.layout.pending);

  detail::SqliteTransaction transaction(journal.database);
  report.demoted = verify_committed_tasks(journal.database, journal.layout);
  report.tasks = reset_incomplete_tasks(journal.database);
  report.abandoned_attempts = abandon_open_attempts(journal.database);
  journal.database.prepare("UPDATE build_session SET status = ?1 WHERE id = ?2")
      .bind(1, build_session_status_name(BuildSessionStatus::active))
      .bind(2, journal.manifest.build_session_id)
      .run();
  transaction.commit();

  // After the commit, so a crash here cannot delete bytes a row still needs.
  report.discarded_completed_blobs = discard_unreferenced_completed(journal.database, journal.layout);
  return report;
}

}  // namespace svp::exec
