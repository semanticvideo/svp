#include "svp/builder/build_run_report.hpp"
#include "svp/package/output_directory.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace svp::builder {
namespace {

// Report identity. Bump the version when a field changes meaning or is
// removed; adding fields keeps the version.
constexpr std::string_view kRunReportSchema = "svp.builder.run_report";
constexpr int kRunReportSchemaVersion = 1;

constexpr std::string_view kClockDescription =
    "steady_clock milliseconds since the command started; the same clock as "
    "progress event t_ms";
constexpr std::string_view kCpuScopeDescription =
    "process-wide getrusage(RUSAGE_SELF) samples taken at span start and end; "
    "concurrent spans share process CPU time; children_* fields are "
    "getrusage(RUSAGE_CHILDREN) for terminated subprocesses such as ffmpeg";

std::string_view span_end_name(StageSpanEnd end) {
  switch (end) {
    case StageSpanEnd::completed:
      return "completed";
    case StageSpanEnd::failed:
      return "failed";
    case StageSpanEnd::unfinished:
      return "unfinished";
  }
  return "unfinished";
}

nlohmann::json resources_json(const ProcessResourceSample& sample) {
  return {{"user_cpu_ms", sample.user_cpu_ms},
          {"system_cpu_ms", sample.system_cpu_ms},
          {"children_user_cpu_ms", sample.children_user_cpu_ms},
          {"children_system_cpu_ms", sample.children_system_cpu_ms},
          {"peak_rss_bytes", sample.peak_rss_bytes}};
}

nlohmann::json span_json(const StageSpan& span, const BuildRunSummary& summary) {
  // Spans still open when the command ended are closed at the report time.
  const std::int64_t end_t_ms = span.end_t_ms.value_or(summary.total_wall_ms);
  const std::optional<ProcessResourceSample> end_resources =
      span.end_t_ms ? span.end_resources : summary.final_resources;

  nlohmann::json value = {
      {"start_t_ms", span.start_t_ms},
      {"end_t_ms", end_t_ms},
      {"wall_ms", end_t_ms - span.start_t_ms},
      {"end", std::string(span_end_name(span.end))},
  };
  if (!span.scope_id.empty()) value["scope_id"] = span.scope_id;
  if (span.start_resources && end_resources) {
    value["cpu"] = {
        {"user_ms_start", span.start_resources->user_cpu_ms},
        {"user_ms_end", end_resources->user_cpu_ms},
        {"user_ms", end_resources->user_cpu_ms - span.start_resources->user_cpu_ms},
        {"system_ms_start", span.start_resources->system_cpu_ms},
        {"system_ms_end", end_resources->system_cpu_ms},
        {"system_ms",
         end_resources->system_cpu_ms - span.start_resources->system_cpu_ms},
        {"children_user_ms", end_resources->children_user_cpu_ms -
                                 span.start_resources->children_user_cpu_ms},
        {"children_system_ms",
         end_resources->children_system_cpu_ms -
             span.start_resources->children_system_cpu_ms},
    };
    value["peak_rss_bytes_at_end"] = end_resources->peak_rss_bytes;
  }
  return value;
}

nlohmann::json stages_json(const std::vector<StageSpan>& spans,
                           const BuildRunSummary& summary) {
  // One entry per stage id, in order of first start.
  nlohmann::json stages = nlohmann::json::array();
  std::vector<ProgressStageId> order;
  for (const StageSpan& span : spans) {
    if (std::find(order.begin(), order.end(), span.stage) == order.end()) {
      order.push_back(span.stage);
    }
  }
  for (const ProgressStageId stage : order) {
    nlohmann::json stage_spans = nlohmann::json::array();
    std::int64_t wall_ms_sum = 0;
    for (const StageSpan& span : spans) {
      if (span.stage != stage) continue;
      nlohmann::json value = span_json(span, summary);
      wall_ms_sum += value.at("wall_ms").get<std::int64_t>();
      stage_spans.push_back(std::move(value));
    }
    stages.push_back({
        {"stage", std::string(progress_stage_id(stage))},
        {"stage_label", std::string(progress_stage_label(stage))},
        {"occurrence_count", stage_spans.size()},
        {"wall_ms_sum", wall_ms_sum},
        {"spans", std::move(stage_spans)},
    });
  }
  return stages;
}

}  // namespace

std::string render_build_run_report_json(const BuildRunReportRecorder& recorder,
                                         const BuildRunSummary& summary) {
  const bool succeeded = summary.exit_code && *summary.exit_code == 0;
  nlohmann::json report = {
      {"schema", std::string(kRunReportSchema)},
      {"schema_version", kRunReportSchemaVersion},
      {"command", summary.command},
      {"status", succeeded ? "succeeded" : "failed"},
      {"exit_code", summary.exit_code ? nlohmann::json(*summary.exit_code)
                                      : nlohmann::json(nullptr)},
      {"clock", std::string(kClockDescription)},
      {"cpu_scope", std::string(kCpuScopeDescription)},
      {"total_wall_ms", summary.total_wall_ms},
      {"event_count", summary.event_count},
      {"unmatched_terminal_events", recorder.unmatched_terminal_events()},
      {"stages", stages_json(recorder.spans(), summary)},
  };
  if (summary.final_resources) {
    report["process"] = resources_json(*summary.final_resources);
  }
  if (summary.thread_plan) {
    report["thread_plan"] =
        svp::models::thread_plan_resolution_to_json(*summary.thread_plan);
  }
  return report.dump(2) + "\n";
}

void write_build_run_report(const std::filesystem::path& path,
                            const BuildRunReportRecorder& recorder,
                            const BuildRunSummary& summary) {
  const std::string text = render_build_run_report_json(recorder, summary);
  svp::package::ensure_parent_directory(path);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  output.flush();
  if (!output) {
    throw std::runtime_error("unable to write run report: " + path.string());
  }
}

}  // namespace svp::builder
