#include "svp/exec/journal_result_commit_sink.hpp"

#include "committed_result_record.hpp"
#include "svp/exec/exec_error.hpp"

#include <limits>
#include <span>
#include <vector>

namespace svp::exec {
namespace {

// The edge from `state` toward result_received (task_state.hpp), or nullopt
// when result_received cannot be reached from it.
std::optional<TaskState> next_toward_result(TaskState state) {
  switch (state) {
    case TaskState::planned:
    case TaskState::failed_retryable:
      return TaskState::ready;
    case TaskState::ready:
    case TaskState::leased:
      return TaskState::running;
    case TaskState::running:
      return TaskState::result_received;
    case TaskState::result_received:
    case TaskState::committed:
    case TaskState::failed_permanent:
      break;
  }
  return std::nullopt;
}

void advance_to_result_received(RecoveryJournal& journal, const std::string& task_id) {
  std::optional<TaskState> state = journal.task_state(task_id);
  if (!state) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task `" + task_id + "` is not recorded in the recovery journal");
  }
  while (*state != TaskState::result_received) {
    const std::optional<TaskState> next = next_toward_result(*state);
    if (!next) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "task `" + task_id + "` is " + std::string(task_state_name(*state)) +
                          " in the recovery journal and cannot commit a result");
    }
    journal.set_task_state(task_id, *next);
    state = next;
  }
}

ArtifactProvenanceRecord provenance_of(const TaskSpec& spec) {
  ArtifactProvenanceRecord provenance{
      .processor_id = spec.task_type + "@" + std::to_string(spec.task_type_version),
      .parameters_blake3 = spec.parameters_blake3,
      .model_refs = {}};
  for (const TaskModelRef& model : spec.model_refs) {
    provenance.model_refs.push_back(model.model_bundle_id);
  }
  return provenance;
}

void require_result_of(const TaskSpec& spec, const CommittedResult& committed,
                       const std::string& journal_session) {
  const TaskResult& result = committed.result;
  if (spec.build_session_id != journal_session) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task `" + spec.task_id + "` belongs to build session `" +
                        spec.build_session_id + "`, the journal to `" + journal_session + "`");
  }
  if (result.task_id != spec.task_id || result.status != TaskStatus::succeeded ||
      committed.payloads.size() != result.outputs.size()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "the result handed to the journal for task `" + spec.task_id +
                        "` is not its verified success");
  }
  if (result.attempt > std::numeric_limits<std::uint32_t>::max()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "task `" + spec.task_id + "` attempt number exceeds the journal's range");
  }
}

TaskAttemptRecord attempt_row(const TaskResult& result) {
  const TaskExecution& execution = result.execution;
  return TaskAttemptRecord{
      .task_id = result.task_id,
      .attempt = static_cast<std::uint32_t>(result.attempt),
      .worker_session_id = execution.worker_session_id,
      .queue_ms = execution.timing_ms.queue,
      .compute_ms = execution.timing_ms.compute,
      .cpu_ms = execution.cpu_ms.user + execution.cpu_ms.system,
      .peak_rss_bytes = execution.peak_rss_bytes,
      .outcome = AttemptOutcome::succeeded};
}

}  // namespace

std::string journal_result_artifact_id(std::string_view task_id) {
  return std::string(task_id) + ".result";
}

std::string journal_output_artifact_id(std::string_view task_id, std::size_t output_index) {
  return std::string(task_id) + ".output." + std::to_string(output_index);
}

JournalResultCommitSink::JournalResultCommitSink(RecoveryJournal& journal)
    : journal_(journal) {}

void JournalResultCommitSink::commit(const TaskSpec& spec, const CommittedResult& committed) {
  require_result_of(spec, committed, journal_.build_session_id());
  const TaskResult& result = committed.result;
  advance_to_result_received(journal_, spec.task_id);

  const ArtifactProvenanceRecord provenance = provenance_of(spec);
  const std::string record =
      detail::encode_committed_result_record(result, committed.executor_id);
  TaskCommit commit{.task_id = spec.task_id, .output_blake3 = result.output_digest};
  for (std::size_t index = 0; index < result.outputs.size(); ++index) {
    commit.artifacts.push_back(JournalArtifactInput{
        .artifact_id = journal_output_artifact_id(spec.task_id, index),
        .blake3 = result.outputs[index].blake3,
        .content = std::span<const std::byte>(committed.payloads[index]),
        .provenance = provenance});
  }
  commit.artifacts.push_back(JournalArtifactInput{
      .artifact_id = journal_result_artifact_id(spec.task_id),
      .blake3 = blake3_digest(record),
      .content = std::as_bytes(std::span(record.data(), record.size())),
      .provenance = provenance});
  journal_.commit_task(commit);
  // After the commit: a crash in between leaves a committed task without its
  // (diagnostic) attempt row, never an attempt row claiming an uncommitted
  // success.
  journal_.record_task_attempt(attempt_row(result));
  ++committed_count_;
}

}  // namespace svp::exec
