// load_committed_results: reads committed results back out of the journal.

#include "committed_result_record.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/journal_result_commit_sink.hpp"
#include "svp/exec/journal_scheduler_resume.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace svp::exec {
namespace {

const JournalArtifactRecord& find_artifact(const JournalCommittedTask& task,
                                           const std::string& artifact_id) {
  const auto found = std::find_if(
      task.artifacts.begin(), task.artifacts.end(),
      [&](const JournalArtifactRecord& artifact) { return artifact.artifact_id == artifact_id; });
  if (found == task.artifacts.end()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "committed task `" + task.task_id + "` has no journal artifact `" +
                        artifact_id + "`");
  }
  return *found;
}

// Every read verifies (RC2 §5.15 rule 5), even right after resume() did.
std::vector<std::byte> read_verified(const JournalArtifactRecord& artifact) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(artifact.path, error);
  std::vector<std::byte> bytes;
  bool read = !error && size == artifact.byte_length;
  if (read) {
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream input(artifact.path, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    read = input.gcount() == static_cast<std::streamsize>(bytes.size());
  }
  if (!read || blake3_digest(bytes) != artifact.blake3) {
    throw ExecError(ExecErrorCode::digest_mismatch,
                    "journal artifact `" + artifact.artifact_id + "` (" +
                        artifact.path.string() + ") does not verify");
  }
  return bytes;
}

CommittedResult read_committed_result(const JournalCommittedTask& task, const TaskSpec& spec) {
  const std::vector<std::byte> record_bytes =
      read_verified(find_artifact(task, journal_result_artifact_id(spec.task_id)));
  detail::CommittedResultRecord record = detail::decode_committed_result_record(
      std::string_view(reinterpret_cast<const char*>(record_bytes.data()), record_bytes.size()));
  const TaskResult& result = record.result;
  if (result.task_id != spec.task_id || result.status != TaskStatus::succeeded ||
      result.output_digest != task.output_blake3 ||
      task.artifacts.size() != result.outputs.size() + 1) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "journal record of committed task `" + spec.task_id +
                        "` does not match its task row and artifacts");
  }
  std::vector<FramePayload> payloads;
  for (std::size_t index = 0; index < result.outputs.size(); ++index) {
    const ArtifactRef& ref = result.outputs[index];
    const JournalArtifactRecord& artifact =
        find_artifact(task, journal_output_artifact_id(spec.task_id, index));
    if (artifact.blake3 != ref.blake3 || artifact.byte_length != ref.bytes) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "journal artifact `" + artifact.artifact_id +
                          "` is not the output its record names");
    }
    payloads.push_back(read_verified(artifact));
  }
  return CommittedResult{.result = std::move(record.result),
                         .payloads = std::move(payloads),
                         .executor_id = std::move(record.executor_id)};
}

}  // namespace

std::vector<CommittedResult> load_committed_results(const RecoveryJournal& journal,
                                                    const TaskGraph& graph) {
  std::vector<CommittedResult> results;
  for (std::size_t index = 0; index < graph.size(); ++index) {
    const TaskSpec& spec = graph.node(index).spec;
    if (const std::optional<JournalCommittedTask> task = journal.committed_task(spec.task_id)) {
      results.push_back(read_committed_result(*task, spec));
    }
  }
  return results;
}

}  // namespace svp::exec
