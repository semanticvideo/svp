#include "svp/builder/build_progress.hpp"
#include "svp/builder/build_run_report.hpp"
#include "svp/builder/progress_timeline.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

using svp::builder::ProgressEvent;
using svp::builder::ProgressStageId;

// Deterministic sampler: every call advances CPU and RSS by fixed steps so
// span deltas are predictable.
constexpr std::int64_t kUserStepMs = 10;
constexpr std::int64_t kSystemStepMs = 2;
constexpr std::int64_t kChildUserStepMs = 3;
constexpr std::int64_t kRssStepBytes = 1024;

svp::builder::ProcessResourceSampler stepping_sampler() {
  auto calls = std::make_shared<std::int64_t>(0);
  return [calls]() -> std::optional<svp::builder::ProcessResourceSample> {
    ++*calls;
    return svp::builder::ProcessResourceSample{
        .user_cpu_ms = *calls * kUserStepMs,
        .system_cpu_ms = *calls * kSystemStepMs,
        .children_user_cpu_ms = *calls * kChildUserStepMs,
        .peak_rss_bytes = *calls * kRssStepBytes,
    };
  };
}

ProgressEvent at(ProgressEvent event, std::int64_t t_ms) {
  event.t_ms = t_ms;
  return event;
}

ProgressEvent scoped(ProgressEvent event, std::string scope) {
  return svp::builder::with_progress_scope(std::move(event), scope, scope);
}

const nlohmann::json& stage_entry(const nlohmann::json& report,
                                  std::string_view stage) {
  for (const auto& entry : report.at("stages")) {
    if (entry.at("stage") == stage) return entry;
  }
  assert(false && "stage missing from run report");
  return report;
}

void feed_representative_build(svp::builder::BuildRunReportRecorder& recorder) {
  using svp::builder::make_stage_completed;
  using svp::builder::make_stage_failed;
  using svp::builder::make_stage_started;
  recorder.emit(at(make_stage_started(ProgressStageId::media_probe), 0));
  recorder.emit(at(make_stage_completed(ProgressStageId::media_probe), 10));
  // Overlapping lanes.
  recorder.emit(at(make_stage_started(ProgressStageId::asr), 10));
  recorder.emit(at(make_stage_started(ProgressStageId::ocr), 12));
  recorder.emit(at(svp::builder::make_stage_progress(ProgressStageId::asr, 1, 2,
                                                     "chunks"),
                   15));
  // A stage that occurs more than once.
  recorder.emit(at(make_stage_started(ProgressStageId::validate), 20));
  recorder.emit(at(make_stage_completed(ProgressStageId::validate), 25));
  recorder.emit(at(make_stage_started(ProgressStageId::validate), 30));
  recorder.emit(at(make_stage_completed(ProgressStageId::validate), 34));
  recorder.emit(at(make_stage_completed(ProgressStageId::ocr), 40));
  recorder.emit(at(make_stage_completed(ProgressStageId::asr), 50));
  recorder.emit(at(make_stage_started(ProgressStageId::identity), 51));
  recorder.emit(at(make_stage_failed(ProgressStageId::identity, "bad"), 55));
  // Terminal event without a start, and an event that was never stamped.
  recorder.emit(at(make_stage_completed(ProgressStageId::depth), 56));
  recorder.emit(make_stage_started(ProgressStageId::embeddings));
  // Still open when the command ends.
  recorder.emit(at(make_stage_started(ProgressStageId::package_write), 60));
}

void test_report_records_every_span_with_timing_and_cpu() {
  svp::builder::BuildRunReportRecorder recorder(stepping_sampler());
  feed_representative_build(recorder);
  const svp::builder::BuildRunSummary summary{
      .command = "build",
      .exit_code = 3,
      .total_wall_ms = 70,
      .event_count = 16,
      .final_resources = svp::builder::ProcessResourceSample{
          .user_cpu_ms = 500, .system_cpu_ms = 50, .peak_rss_bytes = 4096},
  };
  const auto report = nlohmann::json::parse(
      svp::builder::render_build_run_report_json(recorder, summary));

  assert(report.at("schema") == "svp.builder.run_report");
  assert(report.at("schema_version") == 1);
  assert(report.at("command") == "build");
  assert(report.at("status") == "failed");
  assert(report.at("exit_code") == 3);
  assert(report.at("total_wall_ms") == 70);
  assert(report.at("event_count") == 16);
  assert(report.at("unmatched_terminal_events") == 1);
  assert(report.at("process").at("peak_rss_bytes") == 4096);
  assert(report.at("process").at("user_cpu_ms") == 500);
  assert(report.at("process").contains("children_user_cpu_ms"));

  // Stage entries appear in order of first start; unstamped events are absent.
  std::vector<std::string> order;
  for (const auto& entry : report.at("stages")) {
    order.push_back(entry.at("stage").get<std::string>());
  }
  assert((order == std::vector<std::string>{"media_probe", "asr", "ocr",
                                            "validate", "identity",
                                            "package_write"}));

  const auto& validate = stage_entry(report, "validate");
  assert(validate.at("occurrence_count") == 2);
  assert(validate.at("wall_ms_sum") == 9);
  assert(validate.at("spans").at(0).at("start_t_ms") == 20);
  assert(validate.at("spans").at(0).at("wall_ms") == 5);
  assert(validate.at("spans").at(1).at("wall_ms") == 4);

  const auto& asr = stage_entry(report, "asr").at("spans").at(0);
  assert(asr.at("start_t_ms") == 10);
  assert(asr.at("end_t_ms") == 50);
  assert(asr.at("wall_ms") == 40);
  assert(asr.at("end") == "completed");
  // Sampler calls: probe start/end (1,2), asr start (3), ... asr end (10).
  assert(asr.at("cpu").at("user_ms_start") == 3 * kUserStepMs);
  assert(asr.at("cpu").at("user_ms_end") == 10 * kUserStepMs);
  assert(asr.at("cpu").at("user_ms") == 7 * kUserStepMs);
  assert(asr.at("cpu").at("system_ms") == 7 * kSystemStepMs);
  assert(asr.at("cpu").at("children_user_ms") == 7 * kChildUserStepMs);
  assert(asr.at("cpu").at("children_system_ms") == 0);
  assert(asr.at("peak_rss_bytes_at_end") == 10 * kRssStepBytes);
  assert(!asr.contains("scope_id"));

  assert(stage_entry(report, "identity").at("spans").at(0).at("end") ==
         "failed");
  const auto& open = stage_entry(report, "package_write").at("spans").at(0);
  assert(open.at("end") == "unfinished");
  assert(open.at("end_t_ms") == 70);
  assert(open.at("wall_ms") == 10);
  assert(open.at("cpu").at("user_ms_end") == 500);
}

void test_report_status_follows_exit_code() {
  svp::builder::BuildRunReportRecorder recorder(stepping_sampler());
  auto succeeded = nlohmann::json::parse(svp::builder::render_build_run_report_json(
      recorder, {.command = "interlace create", .exit_code = 0}));
  assert(succeeded.at("status") == "succeeded");
  assert(succeeded.at("exit_code") == 0);
  assert(succeeded.at("stages").empty());
  assert(!succeeded.contains("process"));

  auto aborted = nlohmann::json::parse(svp::builder::render_build_run_report_json(
      recorder, {.command = "build", .exit_code = std::nullopt}));
  assert(aborted.at("status") == "failed");
  assert(aborted.at("exit_code").is_null());
}

void test_same_stage_in_different_scopes_is_tracked_separately() {
  svp::builder::BuildRunReportRecorder recorder(stepping_sampler());
  using svp::builder::make_stage_completed;
  using svp::builder::make_stage_started;
  recorder.emit(at(scoped(make_stage_started(ProgressStageId::batch_item), "a"), 0));
  recorder.emit(at(scoped(make_stage_started(ProgressStageId::batch_item), "b"), 5));
  recorder.emit(at(scoped(make_stage_completed(ProgressStageId::batch_item), "a"), 20));
  recorder.emit(at(scoped(make_stage_completed(ProgressStageId::batch_item), "b"), 30));
  const auto spans = recorder.spans();
  assert(spans.size() == 2);
  assert(spans[0].scope_id == "a" && *spans[0].end_t_ms == 20);
  assert(spans[1].scope_id == "b" && *spans[1].end_t_ms == 30);
  assert(recorder.unmatched_terminal_events() == 0);
}

void test_report_round_trips_through_file_with_real_timeline() {
  auto recorder = std::make_shared<svp::builder::BuildRunReportRecorder>();
  auto timeline = svp::builder::make_timestamped_progress_sink({recorder});
  timeline->emit(svp::builder::make_stage_started(ProgressStageId::asr));
  timeline->emit(svp::builder::make_stage_completed(ProgressStageId::asr));
  const svp::builder::BuildRunSummary summary{
      .command = "build",
      .exit_code = 0,
      .total_wall_ms = timeline->elapsed_ms(),
      .event_count = timeline->emitted_count(),
      .final_resources = svp::builder::sample_process_resources(),
  };
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("svp-run-report-test-" + std::to_string(::getpid())) / "nested" /
      "report.json";
  svp::builder::write_build_run_report(path, *recorder, summary);
  std::ifstream input(path);
  const auto report = nlohmann::json::parse(input);
  assert(report.at("status") == "succeeded");
  assert(report.at("event_count") == 2);
  const auto& span = stage_entry(report, "asr").at("spans").at(0);
  assert(span.at("end") == "completed");
  assert(span.at("wall_ms").get<std::int64_t>() >= 0);
  assert(span.contains("cpu"));
  assert(report.at("process").at("peak_rss_bytes").get<std::int64_t>() > 0);
  std::filesystem::remove_all(path.parent_path().parent_path());
}

}  // namespace

int main() {
  test_report_records_every_span_with_timing_and_cpu();
  test_report_status_follows_exit_code();
  test_same_stage_in_different_scopes_is_tracked_separately();
  test_report_round_trips_through_file_with_real_timeline();
  return 0;
}
