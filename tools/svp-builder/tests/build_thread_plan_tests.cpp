#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_run_report.hpp"
#include "svp/builder/build_thread_plan.hpp"
#include "svp/vision/inference_performance.hpp"
#include "pipeline_input_fixture.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using svp::models::ThreadPlan;
using svp::models::ThreadPlanSource;

svp::models::EnvironmentLookup lookup_from(
    std::map<std::string, std::string> values) {
  return [values = std::move(values)](
             std::string_view name) -> std::optional<std::string> {
    const auto found = values.find(std::string(name));
    if (found == values.end()) return std::nullopt;
    return found->second;
  };
}

const svp::models::EnvironmentLookup kNoEnvironment = lookup_from({});

// A plan a coordinator might send: every value differs from a local plan.
ThreadPlan coordinator_plan() {
  ThreadPlan plan = svp::models::resolve_local_thread_plan({6}, 4);
  plan.ocr_recognition = {.intra_op = 2, .inter_op = 2};
  plan.whisper.decode = 3;
  return plan;
}

void test_local_plan_follows_host_and_ocr_profile() {
  for (const std::string profile : {"serial", "background", "conservative", "fast"}) {
    for (const unsigned logical : {4U, 10U, 16U}) {
      svp::builder::BuildPipelineOptions options;
      options.performance.ocr_performance_profile = profile;
      const auto resolution = svp::builder::resolve_build_thread_plan(
          options, {logical}, kNoEnvironment, true);
      assert(resolution.source == ThreadPlanSource::host);
      assert(resolution.host.logical_cpus == logical);
      assert(resolution.overrides.empty());
      assert(resolution.plan ==
             svp::models::resolve_local_thread_plan(
                 {logical},
                 svp::vision::recognition_workers_for_ocr_profile(profile)));
    }
  }
}

void test_supplied_plan_does_not_depend_on_host() {
  svp::builder::BuildPipelineOptions options;
  options.thread_plan = coordinator_plan();
  const auto small = svp::builder::resolve_build_thread_plan(
      options, {8}, kNoEnvironment, false);
  const auto large = svp::builder::resolve_build_thread_plan(
      options, {24}, kNoEnvironment, false);
  assert(small.source == ThreadPlanSource::supplied);
  assert(small.plan == coordinator_plan());
  assert(large.plan == small.plan);
}

void test_environment_overrides_apply_on_top_and_are_recorded() {
  svp::builder::BuildPipelineOptions options;
  options.thread_plan = coordinator_plan();
  const auto resolution = svp::builder::resolve_build_thread_plan(
      options, {10},
      lookup_from({{"SVP_OCR_REC_ONNX_INTRA_OP_THREADS", "7"},
                   {"SVP_OCR_RECOGNITION_PARALLEL_WORKERS", "5"}}),
      /*diagnostic_overrides_enabled=*/false);
  assert(resolution.plan.ocr_recognition.intra_op == 7);
  assert(resolution.plan.ocr_recognition_workers ==
         coordinator_plan().ocr_recognition_workers);
  assert(resolution.overrides.size() == 1);
  assert(resolution.overrides[0].environment_variable ==
         "SVP_OCR_REC_ONNX_INTRA_OP_THREADS");
}

void test_invalid_supplied_plan_is_rejected() {
  svp::builder::BuildPipelineOptions options;
  ThreadPlan plan = coordinator_plan();
  plan.sherpa.embedding = 0;
  options.thread_plan = plan;
  bool threw = false;
  try {
    (void)svp::builder::resolve_build_thread_plan(options, {10}, kNoEnvironment,
                                                  false);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
}

nlohmann::json run_media_ingest(const std::filesystem::path& tmp_dir,
                                std::optional<ThreadPlan> plan,
                                svp::builder::BuildPipelineResult& result) {
  const std::filesystem::path output_path = tmp_dir / "output.json";
  svp::builder::BuildPipelineOptions options;
  options.source_path = "test_video.mp4";
  options.probe_json_path =
      svp::builder::test::write_minimal_probe_json(tmp_dir).string();
  options.output_path = output_path;
  options.stop_after = svp::builder::BuildStage::media_ingest;
  options.thread_plan = std::move(plan);
  options.quiet = true;
  result = svp::builder::BuildPipeline{}.run(options);
  std::ifstream input(output_path);
  return nlohmann::json::parse(input);
}

// The pipeline resolves the plan once, returns it, and records it in the
// builder foundation JSON (never in the package).
void test_pipeline_records_the_plan_it_ran_with() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_thread_plan_pipeline_test";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  svp::builder::BuildPipelineResult supplied_result;
  const nlohmann::json supplied =
      run_media_ingest(tmp_dir, coordinator_plan(), supplied_result);
  assert(supplied_result.exit_code == 0);
  assert(supplied_result.thread_plan.has_value());
  assert(supplied_result.thread_plan->plan == coordinator_plan());
  const nlohmann::json& recorded = supplied.at("builder_command").at("thread_plan");
  assert(recorded.at("source") == "supplied");
  assert(svp::models::thread_plan_from_json(recorded.at("plan")) ==
         coordinator_plan());

  svp::builder::BuildPipelineResult host_result;
  const nlohmann::json host = run_media_ingest(tmp_dir, std::nullopt, host_result);
  assert(host_result.exit_code == 0);
  assert(host_result.thread_plan.has_value());
  assert(host.at("builder_command").at("thread_plan").at("source") == "host");

  std::filesystem::remove_all(tmp_dir);
}

void test_run_report_records_thread_plan() {
  svp::builder::BuildRunReportRecorder recorder(
      []() -> std::optional<svp::builder::ProcessResourceSample> {
        return std::nullopt;
      });
  svp::models::ThreadPlanResolution resolution;
  resolution.plan = coordinator_plan();
  resolution.source = ThreadPlanSource::supplied;
  const svp::builder::BuildRunSummary with_plan{
      .command = "build", .exit_code = 0, .thread_plan = resolution};
  const auto report = nlohmann::json::parse(
      svp::builder::render_build_run_report_json(recorder, with_plan));
  assert(report.at("thread_plan") ==
         svp::models::thread_plan_resolution_to_json(resolution));

  const svp::builder::BuildRunSummary without_plan{.command = "build"};
  const auto bare = nlohmann::json::parse(
      svp::builder::render_build_run_report_json(recorder, without_plan));
  assert(!bare.contains("thread_plan"));
}

}  // namespace

int main() {
  test_local_plan_follows_host_and_ocr_profile();
  test_supplied_plan_does_not_depend_on_host();
  test_environment_overrides_apply_on_top_and_are_recorded();
  test_invalid_supplied_plan_is_rejected();
  test_pipeline_records_the_plan_it_ran_with();
  test_run_report_records_thread_plan();
  return 0;
}
