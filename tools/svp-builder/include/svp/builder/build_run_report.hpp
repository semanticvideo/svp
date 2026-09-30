#pragma once

#include "svp/builder/build_progress.hpp"
#include "svp/builder/process_resources.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace svp::builder {

enum class StageSpanEnd { completed, failed, unfinished };

// One started..completed/failed interval of a progress stage. A stage that
// runs several times (for example `validate`) produces one span per run.
struct StageSpan {
  ProgressStageId stage = ProgressStageId::media_probe;
  std::string scope_id;
  std::int64_t start_t_ms = 0;
  std::optional<std::int64_t> end_t_ms;  // empty while the span is open
  StageSpanEnd end = StageSpanEnd::unfinished;
  std::optional<ProcessResourceSample> start_resources;
  std::optional<ProcessResourceSample> end_resources;
};

// Collects stage spans from timeline-stamped progress events (see
// progress_timeline.hpp). Events without `t_ms` are ignored. Spans are keyed
// by (scope_id, stage); a terminal event closes the most recently opened span
// with the same key.
class BuildRunReportRecorder final : public BuildProgressSink {
 public:
  explicit BuildRunReportRecorder(
      ProcessResourceSampler sampler = sample_process_resources);

  void emit(const ProgressEvent& event) override;

  // Spans in start order; open spans have no `end_t_ms`.
  [[nodiscard]] std::vector<StageSpan> spans() const;
  // Terminal events that had no matching open span.
  [[nodiscard]] std::uint64_t unmatched_terminal_events() const;

 private:
  using SpanKey = std::pair<std::string, ProgressStageId>;

  ProcessResourceSampler sampler_;
  mutable std::mutex mutex_;
  std::vector<StageSpan> spans_;
  std::map<SpanKey, std::vector<std::size_t>> open_spans_;
  std::uint64_t unmatched_terminal_events_ = 0;
};

struct BuildRunSummary {
  std::string command;
  // Empty when the command ended by an uncaught exception.
  std::optional<int> exit_code;
  std::int64_t total_wall_ms = 0;
  std::uint64_t event_count = 0;
  std::optional<ProcessResourceSample> final_resources;
};

std::string render_build_run_report_json(const BuildRunReportRecorder& recorder,
                                         const BuildRunSummary& summary);

// Throws std::runtime_error when the report cannot be written.
void write_build_run_report(const std::filesystem::path& path,
                            const BuildRunReportRecorder& recorder,
                            const BuildRunSummary& summary);

}  // namespace svp::builder
