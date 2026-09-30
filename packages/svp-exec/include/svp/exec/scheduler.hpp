#pragma once

#include "svp/exec/attempt_event.hpp"
#include "svp/exec/build_outcome.hpp"
#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/clock.hpp"
#include "svp/exec/executor.hpp"
#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/scheduler_policy.hpp"
#include "svp/exec/task_graph.hpp"

#include <span>

namespace svp::exec {

// Pull-based DAG scheduler (plan §3.5, §4.4; RC2 §5.16, §20.1-§20.2).
//
// Task lifecycle: waiting (dependencies pending) -> ready -> leased(attempt
// n, executor, expiry) -> committed, or back to ready after a failed attempt,
// or failed (build fails). Specifically:
//
//   * Ready tasks are ordered by critical-path length (longest first), then
//     canonical order key; each executor pulls the next task whenever it has
//     a free slot, never exceeding slots().
//   * Every lease lasts lease_duration(policy.lease, est_seconds) and is
//     renewed by heartbeats; an expired lease is a failed attempt.
//   * A result is verified before commit: TaskResult schema and
//     output_digest, task_id, attempt, and each payload's length and BLAKE3.
//     An invalid result is a failed attempt and quarantines the executor.
//   * Failed attempts (retryable failure, invalid result, executor lost,
//     lease expiry) are retried, preferring an executor that has not failed
//     the task; policy.retry.max_attempts failures fail the build naming the
//     task. A non-retryable failure fails the build at once. A failed or
//     invalid result is never committed.
//   * The first verified result for a task is committed to the sink, exactly
//     once. A later verified result with the same output_digest is discarded
//     and counted; a different digest is a determinism incident and fails the
//     build.
//   * The build succeeds only when every task committed and no attempt is
//     still outstanding (outstanding duplicates are waited for so they are
//     compared).
//   * The cancellation token is checked between dispatch rounds; once set,
//     no new lease is granted, outstanding leases are cancelled, and run()
//     returns cancelled.
//
// Executors are started at the beginning of run() and stopped before it
// returns, on every path. The sink and observer are called on the calling
// thread only.
class Scheduler {
 public:
  // Throws ExecError(invalid_value) for an invalid policy.
  Scheduler(SchedulerPolicy policy, const Clock& clock);

  // Throws ExecError(invalid_value) when there are no executors, an executor
  // is null, advertises zero slots, or repeats an id.
  [[nodiscard]] BuildOutcome run(const TaskGraph& graph,
                                 std::span<Executor* const> executors,
                                 ResultCommitSink& sink,
                                 const CancellationToken& cancellation,
                                 const AttemptObserver& observer = {}) const;

 private:
  SchedulerPolicy policy_;
  const Clock& clock_;
};

}  // namespace svp::exec
