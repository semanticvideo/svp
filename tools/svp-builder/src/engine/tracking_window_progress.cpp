#include "engine/tracking_window_progress.hpp"

#include <iostream>
#include <sstream>

namespace svp::builder::engine {
namespace {

constexpr const char* kProgressUnit = "windows";

}  // namespace

TrackingWindowProgress::TrackingWindowProgress(const TrackingWindowPlan& windows,
                                               BuildProgressSink& sink,
                                               const svp::exec::Clock& clock, bool quiet,
                                               const std::set<std::string>& resumed_task_ids)
    : sink_(sink), clock_(clock), quiet_(quiet) {
  total_windows_ = windows.nodes.size();
  for (const svp::exec::TaskNode& node : windows.nodes) {
    if (resumed_task_ids.contains(node.spec.task_id)) {
      ++done_windows_;
    } else {
      pending_.insert(node.spec.task_id);
    }
  }
}

void TrackingWindowProgress::start_locked() {
  if (started_) return;
  started_ = true;
  sink_.emit(make_stage_started(ProgressStageId::visual_tracking));
}

void TrackingWindowProgress::observe(const svp::exec::AttemptEvent& event) {
  using svp::exec::AttemptEventKind;
  const std::lock_guard lock(mutex_);
  if (!pending_.contains(event.task_id)) {
    return;
  }
  switch (event.kind) {
    case AttemptEventKind::leased:
      leased_at_[event.lease_id] = clock_.now();
      start_locked();
      return;
    case AttemptEventKind::committed: {
      TrackingExecutorTotals& totals = totals_[event.executor_id];
      ++totals.windows;
      if (const auto leased = leased_at_.find(event.lease_id); leased != leased_at_.end()) {
        totals.busy += clock_.now() - leased->second;
        leased_at_.erase(leased);
      }
      pending_.erase(event.task_id);
      ++done_windows_;
      sink_.emit(make_stage_progress(ProgressStageId::visual_tracking, done_windows_,
                                     total_windows_, kProgressUnit));
      return;
    }
    case AttemptEventKind::failed:
    case AttemptEventKind::expired:
    case AttemptEventKind::deadline_exceeded:
      leased_at_.erase(event.lease_id);
      ++totals_[event.executor_id].failed_attempts;
      if (!quiet_) {
        std::cerr << "svp-builder: warning: " << event.task_id << " attempt " << event.attempt
                  << " on " << event.executor_id << " did not finish ("
                  << svp::exec::attempt_event_kind_name(event.kind)
                  << (event.detail.empty() ? std::string() : ": " + event.detail)
                  << "); it is retried until its attempts run out\n";
      }
      return;
    case AttemptEventKind::rejected:
      leased_at_.erase(event.lease_id);
      declined_.observe(event);
      return;
    default:
      return;
  }
}

void TrackingWindowProgress::fold_started() {
  const std::lock_guard lock(mutex_);
  start_locked();
}

void TrackingWindowProgress::fold_finished() {
  const std::lock_guard lock(mutex_);
  start_locked();
  if (completed_) return;
  completed_ = true;
  sink_.emit(make_stage_completed(ProgressStageId::visual_tracking));
}

std::string TrackingWindowProgress::summary() const {
  const std::lock_guard lock(mutex_);
  std::ostringstream out;
  for (const auto& [executor, totals] : totals_) {
    out << "svp-builder: tracking windows on " << executor << ": " << totals.windows
        << " windows, busy " << static_cast<double>(totals.busy.count()) / 1000.0 << " s";
    if (totals.failed_attempts > 0) {
      out << ", " << totals.failed_attempts << " attempt(s) lost or failed";
    }
    out << "\n";
  }
  out << declined_.summary("tracking windows");
  return out.str();
}

}  // namespace svp::builder::engine
