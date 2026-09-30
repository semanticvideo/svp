#pragma once

#include "svp/exec/recovery_journal.hpp"
#include "svp/exec/result_commit_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace svp::exec {

// The production ResultCommitSink (plan §4.4 "committed (journal)", §4.6):
// every verified result becomes durable in the RC2 §20.4 recovery journal
// before the scheduler treats its task as committed.
//
// For each result of task T, commit():
//   1. walks T's journal row to result_received along the task_state.hpp
//      edges (the scheduler does not journal leases, so a row may still be
//      planned, ready, leased, running, or failed_retryable);
//   2. commits, in one RecoveryJournal::commit_task call (pending -> fsync ->
//      BLAKE3 verify -> rename, then one transaction that sets `committed`
//      and output_blake3 = the result's output_digest):
//        "<T>.output.<i>"  the bytes of result.outputs[i], for every i
//        "<T>.result"      the committed-result record (the TaskResult and
//                          executor ID as canonical JSON), so resume can hand
//                          reducers exactly this result
//      each with provenance {processor_id "<task_type>@<task_type_version>",
//      the spec's parameters_blake3, its model_bundle_ids};
//   3. records the task_attempt row (plan §4.6) for the committed attempt:
//      outcome succeeded, the worker session, queue and compute ms, CPU ms,
//      and peak RSS from the result's execution record.
// The task's cache key is its task row's cache_key, written when the graph
// was recorded (record_task_graph in journal_scheduler_resume.hpp).
//
// Every failure throws (JournalError from the journal, ExecError for a result
// that does not belong to this journal or task); the scheduler turns that
// into a commit_failed build failure, because the journal is mandatory (RC2
// §20.4) and a result it did not record must not count as committed.
//
// Not thread-safe, like the journal; the scheduler calls it from one thread.
class JournalResultCommitSink final : public ResultCommitSink {
 public:
  explicit JournalResultCommitSink(RecoveryJournal& journal);

  void commit(const TaskSpec& spec, const CommittedResult& committed) override;

  // Results this sink committed.
  [[nodiscard]] std::uint64_t committed_count() const noexcept { return committed_count_; }

 private:
  RecoveryJournal& journal_;
  std::uint64_t committed_count_ = 0;
};

// Artifact IDs the sink commits for a task (record identifiers, since task IDs
// are).
[[nodiscard]] std::string journal_result_artifact_id(std::string_view task_id);
[[nodiscard]] std::string journal_output_artifact_id(std::string_view task_id,
                                                     std::size_t output_index);

}  // namespace svp::exec
