#include "svp/builder/build_run_report.hpp"

#include <utility>

namespace svp::builder {
namespace {

std::optional<ProcessResourceSample> sample_with(
    const ProcessResourceSampler& sampler) {
  return sampler ? sampler() : std::nullopt;
}

}  // namespace

BuildRunReportRecorder::BuildRunReportRecorder(ProcessResourceSampler sampler)
    : sampler_(std::move(sampler)) {}

void BuildRunReportRecorder::emit(const ProgressEvent& event) {
  if (!event.t_ms) return;
  const bool starts = event.kind == ProgressEventKind::stage_started;
  const bool ends = event.kind == ProgressEventKind::stage_completed ||
                    event.kind == ProgressEventKind::stage_failed;
  if (!starts && !ends) return;

  std::lock_guard<std::mutex> lock(mutex_);
  const SpanKey key{event.scope_id, event.stage_id};
  if (starts) {
    open_spans_[key].push_back(spans_.size());
    spans_.push_back(StageSpan{
        .stage = event.stage_id,
        .scope_id = event.scope_id,
        .start_t_ms = *event.t_ms,
        .start_resources = sample_with(sampler_),
    });
    return;
  }

  auto open = open_spans_.find(key);
  if (open == open_spans_.end() || open->second.empty()) {
    ++unmatched_terminal_events_;
    return;
  }
  StageSpan& span = spans_[open->second.back()];
  open->second.pop_back();
  span.end_t_ms = *event.t_ms;
  span.end = event.kind == ProgressEventKind::stage_completed
                 ? StageSpanEnd::completed
                 : StageSpanEnd::failed;
  span.end_resources = sample_with(sampler_);
}

std::vector<StageSpan> BuildRunReportRecorder::spans() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return spans_;
}

std::uint64_t BuildRunReportRecorder::unmatched_terminal_events() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return unmatched_terminal_events_;
}

}  // namespace svp::builder
