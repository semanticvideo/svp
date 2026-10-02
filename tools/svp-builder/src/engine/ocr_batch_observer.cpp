#include "engine/ocr_batch_observer.hpp"

#include <iostream>
#include <sstream>

namespace svp::builder::engine {

OcrBatchObserver::OcrBatchObserver(const OcrFrameBatchPlan& batches, BuildProgressSink& sink,
                                   const svp::exec::Clock& clock, bool quiet,
                                   const std::set<std::string>& resumed_task_ids,
                                   std::function<void()> on_all_committed)
    : sink_(sink), on_all_committed_(std::move(on_all_committed)), clock_(clock), quiet_(quiet) {
  for (std::size_t index = 0; index < batches.nodes.size(); ++index) {
    const std::string& task_id = batches.nodes[index].spec.task_id;
    const std::uint64_t samples = batches.batches[index].count;
    total_samples_ += samples;
    if (resumed_task_ids.contains(task_id)) {
      done_samples_ += samples;
      continue;
    }
    samples_by_task_.emplace(task_id, samples);
    ++pending_batches_;
  }
}

void OcrBatchObserver::observe(const svp::exec::AttemptEvent& event) {
  using svp::exec::AttemptEventKind;
  if (event.kind == AttemptEventKind::executor_quarantined) {
    quarantined_.push_back(event.executor_id);
    if (!quiet_) {
      std::cerr << "svp-builder: warning: executor " << event.executor_id
                << " gets no more work in this build: " << event.detail << "\n";
    }
    return;
  }
  const auto task = samples_by_task_.find(event.task_id);
  if (task == samples_by_task_.end()) {
    return;
  }
  switch (event.kind) {
    case AttemptEventKind::leased:
      leased_at_[event.lease_id] = clock_.now();
      if (!started_) {
        started_ = true;
        sink_.emit(make_stage_started(ProgressStageId::ocr));
      }
      return;
    case AttemptEventKind::committed: {
      OcrExecutorTotals& totals = totals_[event.executor_id];
      ++totals.batches;
      totals.samples += task->second;
      if (const auto leased = leased_at_.find(event.lease_id); leased != leased_at_.end()) {
        totals.busy += clock_.now() - leased->second;
        leased_at_.erase(leased);
      }
      done_samples_ += task->second;
      --pending_batches_;
      sink_.emit(make_stage_progress(ProgressStageId::ocr, done_samples_, total_samples_,
                                     "frames"));
      if (pending_batches_ == 0 && !completed_) {
        completed_ = true;
        sink_.emit(make_stage_completed(ProgressStageId::ocr));
        if (on_all_committed_) on_all_committed_();
      }
      return;
    }
    case AttemptEventKind::failed:
    case AttemptEventKind::expired:
    case AttemptEventKind::deadline_exceeded:
      leased_at_.erase(event.lease_id);
      ++totals_[event.executor_id].failed_attempts;
      ++retried_attempts_;
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
      return;
    default:
      return;
  }
}

std::string OcrBatchObserver::summary() const {
  std::ostringstream out;
  for (const auto& [executor, totals] : totals_) {
    out << "svp-builder: OCR frame batches on " << executor << ": " << totals.batches
        << " batches, " << totals.samples << " frames, busy "
        << static_cast<double>(totals.busy.count()) / 1000.0 << " s";
    if (totals.failed_attempts > 0) {
      out << ", " << totals.failed_attempts << " attempt(s) lost or failed";
    }
    out << "\n";
  }
  return out.str();
}

}  // namespace svp::builder::engine
