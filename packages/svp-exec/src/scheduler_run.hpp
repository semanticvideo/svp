#pragma once

#include "ready_queue.hpp"
#include "scheduler_inbox.hpp"
#include "svp/exec/scheduler.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace svp::exec::detail {

enum class TaskPhase { waiting, ready, leased, committed };

struct TaskRecord {
  TaskPhase phase = TaskPhase::waiting;
  std::uint64_t attempts_started = 0;
  std::uint64_t failed_attempts = 0;
  // Executors that failed an attempt of this task (retries avoid them).
  std::set<std::size_t> avoid;
  std::set<std::string> active_leases;
  std::uint64_t committed_attempt = 0;
  std::string committed_executor_id;
  Blake3Digest committed_output_digest{};
  std::string last_failure;
};

struct LeaseRecord {
  std::size_t task = 0;
  std::size_t executor = 0;
  Lease lease;
  // Order in which the scheduler granted this lease (1, 2, ...); compared
  // with ExecutorRecord::loss_horizon to group losses into events.
  std::uint64_t grant_sequence = 0;
  std::chrono::milliseconds granted_at{0};
  std::chrono::milliseconds expires_at{0};
  // Hard deadline; never renewed (LeasePolicy attempt deadline).
  std::chrono::milliseconds deadline_at{0};
  bool speculative = false;
};

struct ExecutorRecord {
  Executor* executor = nullptr;
  LossQuarantine loss_quarantine = LossQuarantine::after_repeated_losses;
  std::size_t active = 0;
  // Loss events since the last verified result that followed one (see
  // scheduler.hpp, loss quarantine). Quarantines at
  // RetryPolicy::quarantine_after_loss_events.
  std::uint64_t consecutive_loss_events = 0;
  // Grant sequence of the first lease issued after the latest counted loss
  // event. A lease granted before it was already outstanding at that event:
  // its loss belongs to that event and its success is not yet evidence of
  // recovery.
  std::uint64_t loss_horizon = 0;
  bool quarantined = false;
};

// State and decisions of one Scheduler::run(). Single-threaded: executor
// callbacks arrive through the inbox and are applied on the calling thread.
class SchedulerRun {
 public:
  SchedulerRun(const SchedulerPolicy& policy, const Clock& clock, const TaskGraph& graph,
               std::span<Executor* const> executors, ResultCommitSink& sink,
               const CancellationToken& cancellation, const AttemptObserver& observer);

  // Marks resumed results committed. Call once, before execute(); throws
  // ExecError(invalid_value) for a result that is not a verified success of a
  // distinct graph task.
  void apply_resumed(std::span<const CommittedResult> resumed);

  // Runs to completion; executors must already be started with inbox().
  [[nodiscard]] BuildOutcome execute();
  [[nodiscard]] SchedulerInbox& inbox() noexcept { return inbox_; }

 private:
  // scheduler_run.cpp: event handling and lifecycle.
  void apply(InboxEvent event);
  void on_heartbeat(const HeartbeatEvent& event);
  void on_finished(FinishedEvent event);
  void on_failed(const FailedEvent& event);
  void on_verified_success(const LeaseRecord& lease, AttemptOutput output);
  // Ends leases past their expiry (lost worker) or hard deadline (stuck
  // attempt).
  void expire_leases();
  void end_lease_at_deadline(const LeaseRecord& lease);
  void attempt_failed(std::size_t task, std::size_t executor, std::string reason);
  // Loss accounting for after_repeated_losses executors.
  void count_executor_loss(const LeaseRecord& lease);
  void note_executor_recovery(const LeaseRecord& lease);
  void quarantine(std::size_t executor, const std::string& reason);
  void commit(const LeaseRecord& lease, AttemptOutput output);
  void mark_ready_dependents(std::size_t completed_task);
  LeaseRecord release(std::map<std::string, LeaseRecord>::iterator lease);
  void fail_build(BuildFailureKind kind, std::string task_id, std::string message);
  void abandon_outstanding_leases();
  void emit(AttemptEventKind kind, const LeaseRecord& lease, std::string detail = {});
  [[nodiscard]] std::chrono::milliseconds next_wait() const;

  // scheduler_dispatch.cpp: lease granting.
  void dispatch();
  void dispatch_speculative();
  [[nodiscard]] std::optional<std::size_t> pick_task(std::size_t executor) const;
  [[nodiscard]] bool has_other_usable_executor(std::size_t task,
                                               std::size_t executor) const;
  void grant(std::size_t task, std::size_t executor, bool speculative);

  const SchedulerPolicy& policy_;
  const Clock& clock_;
  const TaskGraph& graph_;
  ResultCommitSink& sink_;
  const CancellationToken& cancellation_;
  const AttemptObserver& observer_;

  SchedulerInbox inbox_;
  ReadySet ready_set_;
  ReadyQueue ready_queue_;
  std::vector<TaskRecord> tasks_;
  std::vector<ExecutorRecord> executors_;
  std::map<std::string, LeaseRecord> leases_;
  std::uint64_t next_lease_number_ = 1;
  BuildOutcome outcome_;
};

}  // namespace svp::exec::detail
