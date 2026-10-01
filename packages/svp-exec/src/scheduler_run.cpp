#include "scheduler_run.hpp"

#include "result_verification.hpp"
#include "svp/exec/exec_error.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

namespace svp::exec::detail {

SchedulerRun::SchedulerRun(const SchedulerPolicy& policy, const Clock& clock,
                           const TaskGraph& graph, std::span<Executor* const> executors,
                           ResultCommitSink& sink, const CancellationToken& cancellation,
                           const AttemptObserver& observer)
    : policy_(policy),
      clock_(clock),
      graph_(graph),
      sink_(sink),
      cancellation_(cancellation),
      observer_(observer),
      ready_set_(graph),
      ready_queue_(graph),
      tasks_(graph.size()) {
  for (Executor* executor : executors) {
    executors_.push_back(
        ExecutorRecord{.executor = executor, .loss_quarantine = executor->loss_quarantine()});
  }
}

void SchedulerRun::apply_resumed(std::span<const CommittedResult> resumed) {
  for (const CommittedResult& committed : resumed) {
    const TaskResult& result = committed.result;
    const std::optional<std::size_t> index = graph_.find(result.task_id);
    if (!index) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "resumed result names task `" + result.task_id +
                          "`, which is not in the task graph");
    }
    TaskRecord& task = tasks_[*index];
    if (task.phase == TaskPhase::committed) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "task `" + result.task_id + "` is resumed twice");
    }
    if (const auto defect = find_result_defect(graph_.node(*index).spec, result.attempt,
                                               result, committed.payloads)) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "resumed result for task `" + result.task_id + "` is invalid: " + *defect);
    }
    if (result.status != TaskStatus::succeeded) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "resumed result for task `" + result.task_id + "` is not a success");
    }
    task.phase = TaskPhase::committed;
    task.committed_attempt = result.attempt;
    task.committed_executor_id = committed.executor_id;
    task.committed_output_digest = result.output_digest;
    // Dependents are made ready in execute(), once every resumed task is known.
    static_cast<void>(ready_set_.complete(*index));
    ++outcome_.stats.resumed;
  }
}

BuildOutcome SchedulerRun::execute() {
  // Without resumed results this is ReadySet::initially_ready(). With them, a
  // task is ready when it is not committed and every dependency is.
  for (std::size_t task = 0; task < graph_.size(); ++task) {
    if (tasks_[task].phase != TaskPhase::waiting) {
      continue;
    }
    const auto dependencies = graph_.dependencies(task);
    if (std::all_of(dependencies.begin(), dependencies.end(),
                    [&](std::size_t dependency) { return ready_set_.is_complete(dependency); })) {
      tasks_[task].phase = TaskPhase::ready;
      ready_queue_.insert(task);
    }
  }
  while (!outcome_.failure) {
    if (cancellation_.requested()) {
      outcome_.status = BuildStatus::cancelled;
      break;
    }
    if (ready_set_.all_complete() && leases_.empty()) {
      outcome_.status = BuildStatus::succeeded;
      break;
    }
    dispatch();
    if (leases_.empty() && !any_executor_backing_off()) {
      // Work remains (not all complete) yet nothing could be leased, and no
      // executor is merely declining work for a moment.
      fail_build(BuildFailureKind::no_usable_executor, {},
                 "tasks remain but every executor is quarantined");
      break;
    }
    for (InboxEvent& event : inbox_.wait(next_wait())) {
      apply(std::move(event));
    }
    expire_leases();
  }
  abandon_outstanding_leases();
  return std::move(outcome_);
}

std::chrono::milliseconds SchedulerRun::next_wait() const {
  std::chrono::milliseconds wait = policy_.max_idle_wait;
  const std::chrono::milliseconds now = clock_.now();
  for (const auto& [lease_id, lease] : leases_) {
    const std::chrono::milliseconds next = std::min(lease.expires_at, lease.deadline_at);
    wait = std::min(wait, std::max(std::chrono::milliseconds(0), next - now));
  }
  for (const ExecutorRecord& record : executors_) {
    if (!record.quarantined && record.declined_until > now) {
      wait = std::min(wait, record.declined_until - now);
    }
  }
  return wait;
}

bool SchedulerRun::backing_off(std::size_t executor) const {
  return executors_[executor].declined_until > clock_.now();
}

bool SchedulerRun::any_executor_backing_off() const {
  for (std::size_t executor = 0; executor < executors_.size(); ++executor) {
    if (!executors_[executor].quarantined && backing_off(executor)) {
      return true;
    }
  }
  return false;
}

void SchedulerRun::apply(InboxEvent event) {
  if (outcome_.failure) {
    return;
  }
  if (auto* heartbeat = std::get_if<HeartbeatEvent>(&event)) {
    on_heartbeat(*heartbeat);
  } else if (auto* finished = std::get_if<FinishedEvent>(&event)) {
    on_finished(std::move(*finished));
  } else if (auto* rejected = std::get_if<RejectedEvent>(&event)) {
    on_rejected(*rejected);
  } else {
    on_failed(std::get<FailedEvent>(event));
  }
}

void SchedulerRun::on_heartbeat(const HeartbeatEvent& event) {
  if (const auto found = leases_.find(event.lease_id); found != leases_.end()) {
    found->second.expires_at = clock_.now() + found->second.lease.duration;
  }
}

LeaseRecord SchedulerRun::release(std::map<std::string, LeaseRecord>::iterator lease) {
  LeaseRecord record = std::move(lease->second);
  leases_.erase(lease);
  --executors_[record.executor].active;
  tasks_[record.task].active_leases.erase(record.lease.lease_id);
  return record;
}

void SchedulerRun::on_finished(FinishedEvent event) {
  const auto found = leases_.find(event.lease_id);
  if (found == leases_.end()) {
    return;  // Abandoned (expired, cancelled, or quarantined); never committed.
  }
  const LeaseRecord lease = release(found);
  // A finished attempt frees capacity: an executor backing off after a
  // rejection may take work again.
  executors_[lease.executor].declined_until = std::chrono::milliseconds{0};
  const TaskSpec& spec = graph_.node(lease.task).spec;
  if (const auto defect = find_result_defect(spec, lease.lease.attempt, event.output)) {
    ++outcome_.stats.invalid_results;
    emit(AttemptEventKind::failed, lease, "invalid result: " + *defect);
    quarantine(lease.executor, "returned an invalid result for task `" + spec.task_id +
                                   "`: " + *defect);
    attempt_failed(lease.task, lease.executor, "invalid result: " + *defect);
    return;
  }
  const TaskResult& result = event.output.result;
  if (result.status == TaskStatus::succeeded) {
    on_verified_success(lease, std::move(event.output));
    return;
  }
  const TaskError& error = *result.error;
  const std::string reason = error.code + ": " + error.message;
  emit(AttemptEventKind::failed, lease, reason);
  if (tasks_[lease.task].phase == TaskPhase::committed) {
    return;  // A duplicate failed after another attempt committed.
  }
  if (!error.retryable) {
    fail_build(BuildFailureKind::permanent_task_failure, spec.task_id,
               "task `" + spec.task_id + "` failed permanently on attempt " +
                   std::to_string(lease.lease.attempt) + ": " + reason);
    return;
  }
  attempt_failed(lease.task, lease.executor, reason);
}

void SchedulerRun::on_verified_success(const LeaseRecord& lease, AttemptOutput output) {
  note_executor_recovery(lease);
  TaskRecord& task = tasks_[lease.task];
  if (task.phase != TaskPhase::committed) {
    commit(lease, std::move(output));
    return;
  }
  if (output.result.output_digest == task.committed_output_digest) {
    ++outcome_.stats.duplicates_discarded;
    emit(AttemptEventKind::duplicate_discarded, lease);
    return;
  }
  const std::string& task_id = graph_.node(lease.task).spec.task_id;
  const std::string message =
      "task `" + task_id + "` attempt " + std::to_string(lease.lease.attempt) +
      " produced output digest " + blake3_prefixed(output.result.output_digest) +
      " but attempt " + std::to_string(task.committed_attempt) + " committed " +
      blake3_prefixed(task.committed_output_digest);
  emit(AttemptEventKind::determinism_incident, lease, message);
  outcome_.incidents.push_back(DeterminismIncident{
      .task_id = task_id,
      .committed_attempt = task.committed_attempt,
      .committed_executor_id = task.committed_executor_id,
      .committed_output_digest = task.committed_output_digest,
      .conflicting = CommittedResult{
          .result = std::move(output.result),
          .payloads = std::move(output.payloads),
          .executor_id = std::string(executors_[lease.executor].executor->id())}});
  fail_build(BuildFailureKind::determinism_incident, task_id, message);
}

void SchedulerRun::commit(const LeaseRecord& lease, AttemptOutput output) {
  const TaskSpec& spec = graph_.node(lease.task).spec;
  TaskRecord& task = tasks_[lease.task];
  const Blake3Digest digest = output.result.output_digest;
  try {
    sink_.commit(spec, CommittedResult{
                           .result = std::move(output.result),
                           .payloads = std::move(output.payloads),
                           .executor_id = std::string(executors_[lease.executor].executor->id())});
  } catch (const std::exception& error) {
    fail_build(BuildFailureKind::commit_failed, spec.task_id,
               "committing task `" + spec.task_id + "` failed: " + error.what());
    return;
  }
  task.phase = TaskPhase::committed;
  task.committed_attempt = lease.lease.attempt;
  task.committed_executor_id = std::string(executors_[lease.executor].executor->id());
  task.committed_output_digest = digest;
  ready_queue_.erase(lease.task);
  ++outcome_.stats.committed;
  emit(AttemptEventKind::committed, lease);
  mark_ready_dependents(lease.task);
}

void SchedulerRun::mark_ready_dependents(std::size_t completed_task) {
  for (const std::size_t ready : ready_set_.complete(completed_task)) {
    // A resumed dependent may already be committed while a dependency that
    // resume demoted ran again; it stays committed.
    if (tasks_[ready].phase == TaskPhase::waiting) {
      tasks_[ready].phase = TaskPhase::ready;
      ready_queue_.insert(ready);
    }
  }
}

void SchedulerRun::on_failed(const FailedEvent& event) {
  const auto found = leases_.find(event.lease_id);
  if (found == leases_.end()) {
    return;
  }
  const LeaseRecord lease = release(found);
  const std::string reason =
      std::string(attempt_failure_kind_name(event.kind)) + ": " + event.message;
  emit(AttemptEventKind::failed, lease, reason);
  if (event.kind == AttemptFailureKind::invalid_result) {
    ++outcome_.stats.invalid_results;
    quarantine(lease.executor, reason);
  } else {
    count_executor_loss(lease);
  }
  attempt_failed(lease.task, lease.executor, reason);
}

// The executor declined the lease (admission). Nothing counts: not the task's
// attempts, not the executor's losses, and the task is not marked to avoid
// it. The task is ready again at once for any executor; this one backs off.
void SchedulerRun::on_rejected(const RejectedEvent& event) {
  const auto found = leases_.find(event.lease_id);
  if (found == leases_.end()) {
    return;
  }
  const LeaseRecord lease = release(found);
  ++outcome_.stats.leases_rejected;
  emit(AttemptEventKind::rejected, lease, event.code + ": " + event.message);
  executors_[lease.executor].declined_until = clock_.now() + policy_.rejection_backoff;
  TaskRecord& task = tasks_[lease.task];
  if (task.phase != TaskPhase::committed && task.active_leases.empty()) {
    task.phase = TaskPhase::ready;
    ready_queue_.insert(lease.task);
  }
}

void SchedulerRun::expire_leases() {
  const std::chrono::milliseconds now = clock_.now();
  std::vector<std::string> ended;
  for (const auto& [lease_id, lease] : leases_) {
    if (lease.expires_at <= now || lease.deadline_at <= now) {
      ended.push_back(lease_id);
    }
  }
  for (const std::string& lease_id : ended) {
    const auto found = leases_.find(lease_id);
    if (found == leases_.end() || outcome_.failure) {
      continue;  // Released by an earlier expiry's quarantine.
    }
    if (found->second.deadline_at <= now) {
      end_lease_at_deadline(release(found));
      continue;
    }
    const LeaseRecord lease = release(found);
    ++outcome_.stats.leases_expired;
    emit(AttemptEventKind::expired, lease);
    executors_[lease.executor].executor->lease_expired(lease_id);
    count_executor_loss(lease);
    attempt_failed(lease.task, lease.executor, "lease expired without a heartbeat");
  }
}

// The attempt is alive (its lease was renewed) but has run past its hard
// deadline: cancel it cooperatively and treat it as lost. The executor keeps
// its process; only the attempt is abandoned.
void SchedulerRun::end_lease_at_deadline(const LeaseRecord& lease) {
  const std::string detail =
      "attempt exceeded its hard deadline of " +
      std::to_string((lease.deadline_at - lease.granted_at).count()) + " ms";
  ++outcome_.stats.deadlines_exceeded;
  emit(AttemptEventKind::deadline_exceeded, lease, detail);
  executors_[lease.executor].executor->cancel(lease.lease.lease_id);
  count_executor_loss(lease);
  attempt_failed(lease.task, lease.executor, detail);
}

void SchedulerRun::attempt_failed(std::size_t task_index, std::size_t executor,
                                  std::string reason) {
  TaskRecord& task = tasks_[task_index];
  ++task.failed_attempts;
  task.avoid.insert(executor);
  task.last_failure = std::move(reason);
  if (task.phase == TaskPhase::committed || !task.active_leases.empty() ||
      outcome_.failure) {
    return;  // Committed already, or another attempt is still running.
  }
  const std::string& task_id = graph_.node(task_index).spec.task_id;
  if (task.failed_attempts >= policy_.retry.max_attempts) {
    fail_build(BuildFailureKind::retries_exhausted, task_id,
               "task `" + task_id + "` failed " + std::to_string(task.failed_attempts) +
                   " attempts (max_attempts " +
                   std::to_string(policy_.retry.max_attempts) +
                   "); last failure: " + task.last_failure);
    return;
  }
  ++outcome_.stats.retries;
  task.phase = TaskPhase::ready;
  ready_queue_.insert(task_index);
}

// A lost attempt is a new loss event only when it was granted after the
// executor's latest counted event; otherwise it was already in flight then
// and shares that event's cause (one dropped session loses every lease it
// held). Counting events rather than attempts keeps the quarantine
// independent of how many slots the executor's machine advertises.
void SchedulerRun::count_executor_loss(const LeaseRecord& lease) {
  ExecutorRecord& record = executors_[lease.executor];
  if (record.loss_quarantine == LossQuarantine::never) {
    return;  // Only an invalid result quarantines it (see LossQuarantine).
  }
  if (lease.grant_sequence < record.loss_horizon) {
    return;  // Part of the loss event already counted.
  }
  ++record.consecutive_loss_events;
  record.loss_horizon = next_lease_number_;
  if (record.consecutive_loss_events >= policy_.retry.quarantine_after_loss_events) {
    quarantine(lease.executor, std::to_string(record.consecutive_loss_events) +
                                   " consecutive loss events (lost, expired, or "
                                   "deadline-exceeded attempts) with no verified result "
                                   "in between");
  }
}

// A verified result of an attempt granted after the latest loss event shows
// the executor works again (for example it reconnected after a crash), so
// later losses start a new run of consecutive events.
void SchedulerRun::note_executor_recovery(const LeaseRecord& lease) {
  ExecutorRecord& record = executors_[lease.executor];
  if (lease.grant_sequence >= record.loss_horizon) {
    record.consecutive_loss_events = 0;
  }
}

void SchedulerRun::quarantine(std::size_t executor, const std::string& reason) {
  ExecutorRecord& record = executors_[executor];
  if (record.quarantined) {
    return;
  }
  record.quarantined = true;
  outcome_.stats.quarantined_executors.emplace_back(record.executor->id());
  if (observer_) {
    observer_(AttemptEvent{.kind = AttemptEventKind::executor_quarantined,
                           .executor_id = std::string(record.executor->id()),
                           .detail = reason});
  }
  // Its other leases are abandoned without counting against their tasks;
  // they go back to the queue and avoid this executor.
  std::vector<std::string> abandoned;
  for (const auto& [lease_id, lease] : leases_) {
    if (lease.executor == executor) {
      abandoned.push_back(lease_id);
    }
  }
  for (const std::string& lease_id : abandoned) {
    const LeaseRecord lease = release(leases_.find(lease_id));
    record.executor->cancel(lease_id);
    TaskRecord& task = tasks_[lease.task];
    task.avoid.insert(executor);
    if (task.phase != TaskPhase::committed && task.active_leases.empty()) {
      task.phase = TaskPhase::ready;
      ready_queue_.insert(lease.task);
    }
  }
}

void SchedulerRun::fail_build(BuildFailureKind kind, std::string task_id,
                              std::string message) {
  if (outcome_.failure) {
    return;
  }
  outcome_.status = BuildStatus::failed;
  outcome_.failure =
      BuildFailure{.kind = kind, .task_id = std::move(task_id), .message = std::move(message)};
}

void SchedulerRun::abandon_outstanding_leases() {
  for (auto iterator = leases_.begin(); iterator != leases_.end();) {
    executors_[iterator->second.executor].executor->cancel(iterator->first);
    iterator = leases_.erase(iterator);
  }
}

void SchedulerRun::emit(AttemptEventKind kind, const LeaseRecord& lease,
                        std::string detail) {
  if (!observer_) {
    return;
  }
  observer_(AttemptEvent{.kind = kind,
                         .task_id = graph_.node(lease.task).spec.task_id,
                         .attempt = lease.lease.attempt,
                         .executor_id = std::string(executors_[lease.executor].executor->id()),
                         .lease_id = lease.lease.lease_id,
                         .speculative = lease.speculative,
                         .detail = std::move(detail)});
}

}  // namespace svp::exec::detail
