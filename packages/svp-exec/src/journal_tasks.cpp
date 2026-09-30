// RecoveryJournal task lifecycle: record, transition, commit.

#include "journal_artifact_store.hpp"
#include "recovery_journal_state.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/journal_error.hpp"

#include <algorithm>
#include <set>

namespace svp::exec {
namespace {

std::optional<TaskState> read_task_state(const detail::SqliteDatabase& database,
                                         std::string_view task_id) {
  detail::SqliteStatement query = database.prepare("SELECT status FROM task WHERE task_id = ?1");
  query.bind(1, task_id);
  if (!query.step()) {
    return std::nullopt;
  }
  const std::string status = query.text(0);
  const auto state = parse_task_state(status);
  if (!state) {
    throw JournalError(JournalErrorCode::database_error,
                       "task " + std::string(task_id) + " has unknown status \"" + status + "\"");
  }
  return state;
}

TaskState require_task_state(const detail::SqliteDatabase& database, std::string_view task_id) {
  const auto state = read_task_state(database, task_id);
  if (!state) {
    throw JournalError(JournalErrorCode::unknown_task,
                       "task " + std::string(task_id) + " is not recorded");
  }
  return *state;
}

void validate_commit(const TaskCommit& commit) {
  std::set<std::string_view> ids;
  for (const JournalArtifactInput& artifact : commit.artifacts) {
    detail::require_record_identifier(artifact.artifact_id, "artifact id");
    if (!ids.insert(artifact.artifact_id).second) {
      throw JournalError(JournalErrorCode::invalid_argument,
                         "artifact " + artifact.artifact_id + " appears twice in one commit");
    }
  }
  if (commit.cache_hit && !ids.contains(commit.cache_hit->artifact_id)) {
    throw JournalError(JournalErrorCode::invalid_argument,
                       "cache hit artifact " + commit.cache_hit->artifact_id +
                           " is not one of the committed artifacts");
  }
}

std::string model_refs_json(const std::vector<std::string>& model_refs) {
  return encode_canonical_json(nlohmann::json(model_refs));
}

}  // namespace

void RecoveryJournal::record_task(const JournalTaskRecord& task) {
  detail::require_record_identifier(task.task_id, "task id");
  if (task.task_type.empty()) {
    throw JournalError(JournalErrorCode::invalid_argument,
                       "task " + task.task_id + " has an empty task type");
  }
  for (const std::string& dependency : task.depends_on) {
    detail::require_record_identifier(dependency, "dependency task id");
  }
  detail::SqliteDatabase& database = state().database;
  detail::SqliteTransaction transaction(database);
  detail::SqliteStatement insert = database.prepare(
      "INSERT INTO task (task_id, task_type, status, cache_key) VALUES (?1, ?2, ?3, ?4)");
  insert.bind(1, task.task_id)
      .bind(2, task.task_type)
      .bind(3, task_state_name(TaskState::planned));
  if (task.cache_key) {
    insert.bind(4, blake3_prefixed(*task.cache_key));
  } else {
    insert.bind_null(4);
  }
  insert.run();
  for (const std::string& dependency : task.depends_on) {
    database
        .prepare("INSERT INTO task_dependency (task_id, depends_on_task_id) VALUES (?1, ?2)")
        .bind(1, task.task_id)
        .bind(2, dependency)
        .run();
  }
  transaction.commit();
}

void RecoveryJournal::set_task_state(std::string_view task_id, TaskState next) {
  detail::JournalState& journal = state();
  detail::SqliteTransaction transaction(journal.database);
  const TaskState current = require_task_state(journal.database, task_id);
  if (next == TaskState::committed || !is_task_transition_allowed(current, next)) {
    throw JournalError(JournalErrorCode::invalid_transition,
                       "task " + std::string(task_id) + ": " +
                           std::string(task_state_name(current)) + " -> " +
                           std::string(task_state_name(next)) + " is not allowed" +
                           (next == TaskState::committed ? " (use commit_task)" : ""));
  }
  detail::SqliteStatement update = journal.database.prepare(
      "UPDATE task SET status = ?1, "
      "started_utc = CASE WHEN ?2 IS NULL THEN started_utc ELSE ?2 END "
      "WHERE task_id = ?3");
  update.bind(1, task_state_name(next));
  if (next == TaskState::running) {
    update.bind(2, journal.now());
  } else {
    update.bind_null(2);
  }
  update.bind(3, task_id).run();
  transaction.commit();
}

std::optional<TaskState> RecoveryJournal::task_state(std::string_view task_id) const {
  return read_task_state(state().database, task_id);
}

void RecoveryJournal::commit_task(const TaskCommit& commit) {
  detail::JournalState& journal = state();
  validate_commit(commit);
  const TaskState current = require_task_state(journal.database, commit.task_id);
  if (current != TaskState::result_received) {
    throw JournalError(JournalErrorCode::invalid_transition,
                       "task " + commit.task_id + " is " + std::string(task_state_name(current)) +
                           "; only result_received tasks can be committed");
  }

  // Blobs first: a crash after this point leaves unreferenced completed
  // blobs (removed by resume), never rows that point at missing bytes.
  const std::vector<detail::CommittedBlob> blobs =
      detail::store_task_artifacts(journal.layout, commit.artifacts);

  detail::SqliteTransaction transaction(journal.database);
  for (std::size_t index = 0; index < commit.artifacts.size(); ++index) {
    const JournalArtifactInput& artifact = commit.artifacts[index];
    journal.database
        .prepare(
            "INSERT INTO artifact (artifact_id, task_id, relative_path, blake3, byte_length) "
            "VALUES (?1, ?2, ?3, ?4, ?5)")
        .bind(1, artifact.artifact_id)
        .bind(2, commit.task_id)
        .bind(3, blobs[index].relative_path)
        .bind(4, blake3_hex(artifact.blake3))
        .bind_unsigned(5, blobs[index].bytes)
        .run();
    journal.database
        .prepare(
            "INSERT INTO artifact_provenance "
            "(artifact_id, processor_id, parameters_blake3, model_refs_json) "
            "VALUES (?1, ?2, ?3, ?4)")
        .bind(1, artifact.artifact_id)
        .bind(2, artifact.provenance.processor_id)
        .bind(3, blake3_hex(artifact.provenance.parameters_blake3))
        .bind(4, model_refs_json(artifact.provenance.model_refs))
        .run();
  }
  if (commit.cache_hit) {
    journal.database
        .prepare(
            "INSERT INTO cache_hit (task_id, cache_key, artifact_id, verified) "
            "VALUES (?1, ?2, ?3, ?4)")
        .bind(1, commit.task_id)
        .bind(2, blake3_prefixed(commit.cache_hit->cache_key))
        .bind(3, commit.cache_hit->artifact_id)
        .bind(4, std::int64_t{commit.cache_hit->verified ? 1 : 0})
        .run();
  }
  journal.database
      .prepare(
          "UPDATE task SET status = ?1, output_blake3 = ?2, completed_utc = ?3 "
          "WHERE task_id = ?4")
      .bind(1, task_state_name(TaskState::committed))
      .bind(2, blake3_hex(commit.output_blake3))
      .bind(3, journal.now())
      .bind(4, commit.task_id)
      .run();
  transaction.commit();
}

}  // namespace svp::exec
