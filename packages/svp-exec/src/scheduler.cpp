#include "svp/exec/scheduler.hpp"

#include "scheduler_run.hpp"
#include "svp/exec/exec_error.hpp"

#include <set>
#include <string>

namespace svp::exec {
namespace {

void validate_executors(std::span<Executor* const> executors) {
  if (executors.empty()) {
    throw ExecError(ExecErrorCode::invalid_value, "scheduler needs at least one executor");
  }
  std::set<std::string, std::less<>> ids;
  for (const Executor* executor : executors) {
    if (executor == nullptr || executor->slots() == 0) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "scheduler executors must be non-null with at least one slot");
    }
    if (!ids.emplace(executor->id()).second) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "executor id `" + std::string(executor->id()) + "` is not unique");
    }
  }
}

// Stops every executor on every exit path so no thread or process outlives
// run(), even when the sink or observer throws.
class ExecutorsStopper {
 public:
  explicit ExecutorsStopper(std::span<Executor* const> executors) : executors_(executors) {}
  ~ExecutorsStopper() {
    for (Executor* executor : executors_) {
      executor->stop();
    }
  }
  ExecutorsStopper(const ExecutorsStopper&) = delete;
  ExecutorsStopper& operator=(const ExecutorsStopper&) = delete;

 private:
  std::span<Executor* const> executors_;
};

}  // namespace

Scheduler::Scheduler(SchedulerPolicy policy, const Clock& clock)
    : policy_(policy), clock_(clock) {
  validate_scheduler_policy(policy_);
}

BuildOutcome Scheduler::run(const TaskGraph& graph, std::span<Executor* const> executors,
                            ResultCommitSink& sink, const CancellationToken& cancellation,
                            const AttemptObserver& observer,
                            std::span<const CommittedResult> resumed) const {
  validate_executors(executors);
  detail::SchedulerRun run(policy_, clock_, graph, executors, sink, cancellation, observer);
  // Before any executor starts, so a bad resume input throws with nothing to
  // stop.
  run.apply_resumed(resumed);
  const ExecutorsStopper stopper(executors);
  for (Executor* executor : executors) {
    executor->start(run.inbox());
  }
  return run.execute();
}

}  // namespace svp::exec
