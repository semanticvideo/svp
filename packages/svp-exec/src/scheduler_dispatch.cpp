#include "scheduler_run.hpp"

#include "svp/exec/lease_policy.hpp"

namespace svp::exec::detail {

void SchedulerRun::dispatch() {
  for (std::size_t executor = 0; executor < executors_.size(); ++executor) {
    ExecutorRecord& record = executors_[executor];
    while (!record.quarantined && record.active < record.executor->slots()) {
      const std::optional<std::size_t> task = pick_task(executor);
      if (!task) {
        break;
      }
      grant(*task, executor, false);
    }
  }
  if (policy_.speculative_duplicates && ready_queue_.empty()) {
    dispatch_speculative();
  }
}

// First ready task in queue order this executor should take. With
// prefer_different_executor, a task this executor already failed is left for
// another usable executor, and taken only when none exists.
std::optional<std::size_t> SchedulerRun::pick_task(std::size_t executor) const {
  for (const std::size_t task : ready_queue_) {
    if (!policy_.retry.prefer_different_executor || !tasks_[task].avoid.contains(executor) ||
        !has_other_usable_executor(task, executor)) {
      return task;
    }
  }
  return std::nullopt;
}

bool SchedulerRun::has_other_usable_executor(std::size_t task, std::size_t executor) const {
  for (std::size_t other = 0; other < executors_.size(); ++other) {
    if (other != executor && !executors_[other].quarantined &&
        !tasks_[task].avoid.contains(other)) {
      return true;
    }
  }
  return false;
}

// Plan §4.4 stragglers: an idle executor duplicates the longest-running
// lease of a task that has exactly one attempt in flight on another
// executor. The duplicate is its own attempt; whichever verified result
// arrives first commits and the other is compared against it.
void SchedulerRun::dispatch_speculative() {
  for (std::size_t executor = 0; executor < executors_.size(); ++executor) {
    ExecutorRecord& record = executors_[executor];
    while (!record.quarantined && record.active < record.executor->slots()) {
      const LeaseRecord* oldest = nullptr;
      for (const auto& [lease_id, lease] : leases_) {
        const TaskRecord& task = tasks_[lease.task];
        if (lease.executor == executor || task.phase == TaskPhase::committed ||
            task.active_leases.size() != 1 || task.avoid.contains(executor)) {
          continue;
        }
        if (oldest == nullptr || lease.granted_at < oldest->granted_at) {
          oldest = &lease;
        }
      }
      if (oldest == nullptr) {
        break;
      }
      grant(oldest->task, executor, true);
    }
  }
}

void SchedulerRun::grant(std::size_t task_index, std::size_t executor, bool speculative) {
  const TaskSpec& spec = graph_.node(task_index).spec;
  TaskRecord& task = tasks_[task_index];
  ExecutorRecord& record = executors_[executor];

  LeaseRecord lease;
  lease.task = task_index;
  lease.executor = executor;
  lease.speculative = speculative;
  lease.lease = Lease{.lease_id = "lease_" + std::to_string(next_lease_number_++),
                      .attempt = ++task.attempts_started,
                      .duration = lease_duration(policy_.lease, spec.resources.est_seconds),
                      .heartbeat_interval = policy_.lease.heartbeat_interval};
  lease.granted_at = clock_.now();
  lease.expires_at = lease.granted_at + lease.lease.duration;

  task.phase = TaskPhase::leased;
  task.active_leases.insert(lease.lease.lease_id);
  ready_queue_.erase(task_index);
  ++record.active;
  ++outcome_.stats.attempts_started;
  if (speculative) {
    ++outcome_.stats.speculative_attempts;
  }
  const Lease issued = lease.lease;
  emit(AttemptEventKind::leased, lease);
  leases_.emplace(issued.lease_id, std::move(lease));
  record.executor->assign(spec, issued);
}

}  // namespace svp::exec::detail
