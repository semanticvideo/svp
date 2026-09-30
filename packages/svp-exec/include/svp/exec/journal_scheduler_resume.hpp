#pragma once

#include "svp/exec/journal_resume.hpp"
#include "svp/exec/recovery_journal.hpp"
#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/task_graph.hpp"

#include <span>
#include <vector>

namespace svp::exec {

// Joins the scheduler to the recovery journal across a coordinator restart
// (plan §7.3 "coordinator crash: --resume verifies hashes, expires leases";
// RC2 §20.4 `svp build --resume`).
//
// A build that may be resumed:
//   1. RecoveryJournal::create(), then record_task_graph();
//   2. Scheduler::run with a JournalResultCommitSink.
// After a crash, the next process:
//   1. RecoveryJournal::open(), then resume_scheduler_state();
//   2. Scheduler::run with a JournalResultCommitSink and
//      `resumed = state.committed`: committed tasks are never leased again;
//   3. reducers read load_committed_results() (resumed and new results alike,
//      in graph order); an uninterrupted build reduces the same way, so
//      reduced output does not depend on whether, or where, a build stopped.

// Records every graph node as a `planned` task row with its dependencies and
// cache key. Throws ExecError(invalid_value) when a node's build_session_id
// is not the journal's, and JournalError(duplicate_record) when a task is
// already recorded.
void record_task_graph(RecoveryJournal& journal, const TaskGraph& graph);

// Every graph task the journal holds as `committed`, read back from the
// journal and verified (each artifact's length and BLAKE3; the record's
// task_id and output_digest against the task row), in graph order. Throws
// ExecError(digest_mismatch) for bytes that no longer verify,
// (invalid_value) for a committed task whose record is missing or
// inconsistent, and JournalError(unknown_task) for a graph task the journal
// never recorded.
[[nodiscard]] std::vector<CommittedResult> load_committed_results(
    const RecoveryJournal& journal, const TaskGraph& graph);

struct SchedulerResumeState {
  ResumeReport report;
  // Pass to Scheduler::run as `resumed`. Graph order.
  std::vector<CommittedResult> committed;
};

// RecoveryJournal::resume(current_sources) (source check, pending discard,
// re-verification and demotion of committed outputs, abandoned attempts),
// then checks the journal describes this graph: every journaled task is a
// graph node with the same task type, dependencies, and cache key, so a
// result committed for different work is never reused. Graph nodes the
// journal lacks (a crash while record_task_graph ran) are recorded as
// `planned`. Finally loads the committed results. Throws what resume() and
// load_committed_results() throw, and ExecError(invalid_value) when the
// journal does not describe `graph`.
[[nodiscard]] SchedulerResumeState resume_scheduler_state(
    RecoveryJournal& journal, const TaskGraph& graph,
    std::span<const SourceFingerprintRecord> current_sources);

}  // namespace svp::exec
