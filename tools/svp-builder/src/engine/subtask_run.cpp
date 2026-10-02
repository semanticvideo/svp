#include "engine/subtask_run.hpp"

#include "svp/exec/attempt_event.hpp"
#include "svp/exec/clock.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/scheduler.hpp"
#include "svp/exec/task_type_restricted_executor.hpp"
#include "svp/vision/dispatched_work.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>

namespace svp::builder::engine {
namespace {

// Scheduler rules for dispatched tasks: svp-exec's defaults for small tasks
// that may run on another Mac (lease_policy.hpp, retry_policy.hpp), as for
// OCR frame batches (ocr_execution_policy.hpp): a lease that notices a lost
// worker within the 30 s floor, a hard deadline for a stuck attempt, and up
// to three attempts preferring another executor. A dispatched task writes
// nothing to staging, so running it again elsewhere is always safe.
svp::exec::SchedulerPolicy subtask_policy(const std::string& task_type) {
  svp::exec::SchedulerPolicy policy;
  policy.task_types.emplace(task_type,
                            svp::exec::TaskTypePolicy{.lease = svp::exec::LeasePolicy{},
                                                      .max_attempts = svp::exec::kDefaultMaxAttempts});
  return policy;
}

class OrderedSink final : public svp::exec::ResultCommitSink {
 public:
  explicit OrderedSink(const SubtaskRunRequest& request) : request_(request) {
    for (std::size_t index = 0; index < request.nodes.size(); ++index) {
      index_.emplace(request.nodes[index].spec.task_id, index);
    }
    results_.resize(request.nodes.size());
  }

  void commit(const svp::exec::TaskSpec& spec,
              const svp::exec::CommittedResult& committed) override {
    const std::size_t index = index_.at(spec.task_id);
    results_[index] = committed;
    if (request_.on_committed) {
      request_.on_committed(request_.nodes[index]);
    }
  }

  std::vector<svp::exec::CommittedResult> take() { return std::move(results_); }

 private:
  const SubtaskRunRequest& request_;
  std::map<std::string, std::size_t> index_;
  std::vector<svp::exec::CommittedResult> results_;
};

// Per-executor counts and one stderr line per lost or failed attempt, so a
// worker dropping out is visible while the stage goes on.
class SubtaskReport {
 public:
  SubtaskReport(const SubtaskRunRequest& request, bool report)
      : request_(request), report_(report) {}

  void observe(const svp::exec::AttemptEvent& event) {
    using Kind = svp::exec::AttemptEventKind;
    if (event.kind == Kind::committed) {
      ++tasks_by_executor_[event.executor_id];
      return;
    }
    if (event.kind == Kind::failed || event.kind == Kind::expired ||
        event.kind == Kind::deadline_exceeded) {
      ++failed_attempts_;
      if (report_) {
        std::cerr << "svp-builder: " << event.task_id << " attempt " << event.attempt << " on "
                  << event.executor_id << ": "
                  << svp::exec::attempt_event_kind_name(event.kind)
                  << (event.detail.empty() ? std::string() : " (" + event.detail + ")")
                  << "; it runs again\n";
      }
    }
  }

  void summarize() const {
    if (!report_) {
      return;
    }
    std::ostringstream line;
    line << "svp-builder: " << request_.task_type << ": " << request_.nodes.size() << " task(s)";
    for (const auto& [executor, tasks] : tasks_by_executor_) {
      line << ", " << executor << " " << tasks;
    }
    if (failed_attempts_ > 0) {
      line << ", " << failed_attempts_ << " attempt(s) retried";
    }
    std::cerr << line.str() << "\n";
  }

 private:
  const SubtaskRunRequest& request_;
  bool report_;
  std::map<std::string, std::size_t> tasks_by_executor_;
  std::size_t failed_attempts_ = 0;
};

}  // namespace

std::vector<svp::exec::CommittedResult> run_subtasks(const SubtaskRunRequest& request,
                                                     const VisionDispatchSetup& setup,
                                                     const svp::exec::TaskTypeRegistry& registry,
                                                     svp::exec::TaskArtifactAccess& artifacts) {
  if (request.nodes.empty()) {
    return {};
  }
  const auto capacity = setup.capacity.find(request.task_type);
  if (capacity == setup.capacity.end()) {
    throw svp::vision::DispatchedWorkError(request.task_type + " is not dispatched in this build");
  }
  const std::set<std::string, std::less<>> only_type{request.task_type};
  std::vector<svp::exec::Executor*> executors;
  std::optional<svp::exec::InProcessExecutor> local;
  std::optional<svp::exec::TaskTypeRestrictedExecutor> local_only;
  if (capacity->second.coordinator_slots > 0) {
    local.emplace(registry, artifacts,
                  svp::exec::InProcessExecutorOptions{
                      .executor_id = "in-process." + request.task_type,
                      .threads = capacity->second.coordinator_slots,
                      .worker_session_id = "ws_" + setup.build_session_id + "_" +
                                           request.task_type,
                      .runtime_id = {}});
    local_only.emplace(*local, only_type);
    executors.push_back(&*local_only);
  }
  std::vector<std::unique_ptr<svp::exec::Executor>> workers;
  if (setup.workers) {
    workers = setup.workers->make(request.task_type);
  }
  for (const std::unique_ptr<svp::exec::Executor>& worker : workers) {
    executors.push_back(worker.get());
  }
  if (executors.empty()) {
    throw svp::vision::DispatchedWorkError("no executor can run " + request.task_type);
  }

  const svp::exec::TaskGraph graph(request.nodes);
  OrderedSink sink(request);
  SubtaskReport report(request, setup.report);
  const svp::exec::AttemptObserver observer = [&report](const svp::exec::AttemptEvent& event) {
    report.observe(event);
  };
  const svp::exec::CancellationToken never_cancelled;
  const svp::exec::CancellationToken& cancellation =
      setup.cancellation != nullptr ? *setup.cancellation : never_cancelled;
  const svp::exec::SteadyClock clock;
  const svp::exec::BuildOutcome outcome =
      svp::exec::Scheduler(subtask_policy(request.task_type), clock)
          .run(graph, executors, sink, cancellation, observer);
  report.summarize();
  if (outcome.status == svp::exec::BuildStatus::cancelled) {
    throw svp::vision::DispatchedWorkError(request.task_type + " tasks cancelled");
  }
  if (outcome.status != svp::exec::BuildStatus::succeeded) {
    throw svp::vision::DispatchedWorkError(
        request.task_type + " tasks failed: " +
        (outcome.failure ? outcome.failure->message : std::string("unknown failure")));
  }
  return sink.take();
}

}  // namespace svp::builder::engine
