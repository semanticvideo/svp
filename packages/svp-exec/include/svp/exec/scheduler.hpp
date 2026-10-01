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
//   * Every attempt also has a hard deadline, attempt_deadline(policy.lease,
//     est_seconds) after its grant, that heartbeats do not renew. An attempt
//     still outstanding at its deadline is cancelled (the executor sets its
//     CancellationToken) and treated as lost.
//   * A result is verified before commit: TaskResult schema and
//     output_digest, task_id, attempt, and each payload's length and BLAKE3.
//     An invalid result is a failed attempt and quarantines the executor.
//   * Failed attempts (retryable failure, invalid result, executor lost,
//     lease expiry, deadline) are retried, preferring an executor that has not
//     failed the task; policy.retry.max_attempts failures fail the build
//     naming the task. A non-retryable failure fails the build at once. A
//     failed or invalid result is never committed.
//   * Lost attempts (executor lost, lease expiry, deadline) quarantine an
//     executor only when its loss_quarantine() is after_repeated_losses,
//     after policy.retry.quarantine_after_loss_events consecutive loss
//     events. A loss event is counted when an attempt granted after the
//     executor's latest counted event is lost; losses of attempts that were
//     already outstanding at that event belong to it (a dropped session
//     loses every in-flight lease at once, so one crash of a many-slot
//     worker is one event). A verified result of an attempt granted after
//     the latest event shows the executor recovered and resets its count.
//     An executor that declares LossQuarantine::never (the in-process
//     executor) is quarantined only for invalid results.
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
//   * Resume (plan §7.3 "coordinator crash"): `resumed` holds the verified
//     results an interrupted run already committed (see
//     resume_scheduler_state in journal_scheduler_resume.hpp). Their tasks
//     start committed: they are never leased, never handed to the sink again,
//     and count toward their dependents' readiness. A later duplicate cannot
//     occur because no attempt of theirs is started.
//
// Executors are started at the beginning of run() and stopped before it
// returns, on every path. The sink and observer are called on the calling
// thread only.
class Scheduler {
 public:
  // Throws ExecError(invalid_value) for an invalid policy.
  Scheduler(SchedulerPolicy policy, const Clock& clock);

  // Throws ExecError(invalid_value) when there are no executors, an executor
  // is null, advertises zero slots, or repeats an id, and when a `resumed`
  // result names a task outside the graph, repeats a task, or is not a
  // verified success (schema, output_digest, payload lengths and BLAKE3).
  [[nodiscard]] BuildOutcome run(const TaskGraph& graph,
                                 std::span<Executor* const> executors,
                                 ResultCommitSink& sink,
                                 const CancellationToken& cancellation,
                                 const AttemptObserver& observer = {},
                                 std::span<const CommittedResult> resumed = {}) const;

 private:
  SchedulerPolicy policy_;
  const Clock& clock_;
};

}  // namespace svp::exec
