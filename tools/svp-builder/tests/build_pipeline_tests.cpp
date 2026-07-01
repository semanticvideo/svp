#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_progress.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

void test_supported_stage_names_are_stable_and_ordered() {
  const std::vector<std::string_view> names =
      svp::builder::supported_build_stage_names();

  assert((names == std::vector<std::string_view>{
                       "media-ingest",
                       "audio",
                       "vision-plan",
                       "foundation-color",
                       "foundation-ocr",
                       "package-skeleton",
                   }));
}

void test_stage_name_round_trip() {
  for (std::string_view name : svp::builder::supported_build_stage_names()) {
    const std::optional<svp::builder::BuildStage> stage =
        svp::builder::parse_build_stage(name);
    assert(stage.has_value());
    assert(svp::builder::build_stage_name(*stage) == name);
  }

  assert(!svp::builder::parse_build_stage(""));
  assert(!svp::builder::parse_build_stage("package"));
  assert(!svp::builder::parse_build_stage("foundation-audio"));
}

void test_execution_plan_preserves_existing_stage_conditions() {
  using svp::builder::BuildStage;
  using svp::builder::execution_plan_for_stage;

  auto media_ingest = execution_plan_for_stage(BuildStage::media_ingest);
  assert(!media_ingest.run_audio);
  assert(!media_ingest.run_vision_plan);
  assert(!media_ingest.run_foundation_color);
  assert(!media_ingest.run_foundation_ocr);
  assert(!media_ingest.run_package_skeleton);

  auto audio = execution_plan_for_stage(BuildStage::audio);
  assert(audio.run_audio);
  assert(!audio.run_vision_plan);
  assert(!audio.run_foundation_color);
  assert(!audio.run_foundation_ocr);
  assert(!audio.run_package_skeleton);

  auto vision_plan = execution_plan_for_stage(BuildStage::vision_plan);
  assert(!vision_plan.run_audio);
  assert(vision_plan.run_vision_plan);
  assert(!vision_plan.run_foundation_color);
  assert(!vision_plan.run_foundation_ocr);
  assert(!vision_plan.run_package_skeleton);

  auto foundation_color = execution_plan_for_stage(BuildStage::foundation_color);
  assert(!foundation_color.run_audio);
  assert(!foundation_color.run_vision_plan);
  assert(foundation_color.run_foundation_color);
  assert(!foundation_color.run_foundation_ocr);
  assert(!foundation_color.run_package_skeleton);

  auto foundation_ocr = execution_plan_for_stage(BuildStage::foundation_ocr);
  assert(!foundation_ocr.run_audio);
  assert(!foundation_ocr.run_vision_plan);
  assert(!foundation_ocr.run_foundation_color);
  assert(foundation_ocr.run_foundation_ocr);
  assert(!foundation_ocr.run_package_skeleton);

  auto package_skeleton = execution_plan_for_stage(BuildStage::package_skeleton);
  assert(package_skeleton.run_audio);
  assert(!package_skeleton.run_vision_plan);
  assert(package_skeleton.run_foundation_color);
  assert(!package_skeleton.run_foundation_ocr);
  assert(package_skeleton.run_package_skeleton);
}

void test_default_staging_dir_matches_existing_cli_contract() {
  const std::filesystem::path output_path = "out/example.foundation.json";
  assert(svp::builder::default_staging_dir_for_output(output_path) ==
         std::filesystem::path("out/example.foundation.json.staging"));
}

void test_package_skeleton_output_path_resolution() {
  auto svp_output =
      svp::builder::resolve_package_skeleton_output_paths("out/video.svp");
  assert(svp_output.package_path == std::filesystem::path("out/video.svp"));
  assert(svp_output.json_output_path == std::filesystem::path("out/video.svp.json"));

  auto json_output =
      svp::builder::resolve_package_skeleton_output_paths("out/video.json");
  assert(json_output.package_path == std::filesystem::path("out/video.svp"));
  assert(json_output.json_output_path == std::filesystem::path("out/video.json"));

  auto extensionless_output =
      svp::builder::resolve_package_skeleton_output_paths("out/video");
  assert(extensionless_output.package_path == std::filesystem::path("out/video.svp"));
  assert(extensionless_output.json_output_path == std::filesystem::path("out/video"));
}

}  // namespace

// --- Progress event tests ---

namespace {

class CapturingProgressSink : public svp::builder::BuildProgressSink {
 public:
  void emit(const svp::builder::ProgressEvent& event) override {
    events.push_back(event);
  }

  std::vector<svp::builder::ProgressEvent> events;
};

std::filesystem::path write_minimal_probe_json(
    const std::filesystem::path& dir) {
  const std::filesystem::path probe_path = dir / "probe.json";
  std::ofstream out(probe_path);
  out << R"({
    "format_name": "mov,mp4,m4a,3gp,3g2,mj2",
    "container_timing": {
      "timebase": "1/1000000",
      "start_pts": 0,
      "duration_pts": 3000000
    },
    "video_streams": [
      {
        "id": "vstream_0001",
        "index": 0,
        "codec_name": "h264",
        "width": 1920,
        "height": 1080,
        "pixel_aspect_ratio": "1:1",
        "timing": {
          "timebase": "1/30000",
          "start_pts": 0,
          "duration_pts": 90090,
          "avg_frame_rate": "30000/1001",
          "frame_count": 90
        }
      }
    ],
    "audio_streams": [
      {
        "id": "astream_0001",
        "index": 1,
        "codec_name": "aac",
        "sample_rate": 48000,
        "channels": 2,
        "timing": {
          "timebase": "1/48000",
          "start_pts": 0,
          "duration_pts": 144000
        }
      }
    ]
  })";
  out.close();
  return probe_path;
}

void test_stage_catalog_ids_and_labels_from_one_source() {
  const std::vector<svp::builder::ProgressStageId> stages =
      svp::builder::all_progress_stages();

  assert(!stages.empty());

  for (svp::builder::ProgressStageId stage : stages) {
    const std::string_view id = svp::builder::progress_stage_id(stage);
    const std::string_view label = svp::builder::progress_stage_label(stage);
    assert(!id.empty());
    assert(!label.empty());
  }

  assert(svp::builder::progress_stage_id(
             svp::builder::ProgressStageId::media_probe) == "media_probe");
  assert(svp::builder::progress_stage_label(
             svp::builder::ProgressStageId::media_probe) == "Media Probe");

  assert(svp::builder::progress_stage_id(
             svp::builder::ProgressStageId::package_write) == "package_write");
  assert(svp::builder::progress_stage_label(
             svp::builder::ProgressStageId::package_write) == "Package Write");

  assert(svp::builder::progress_stage_id(
             svp::builder::ProgressStageId::validate) == "validate");
}

void test_event_kind_names() {
  assert(svp::builder::progress_event_kind_name(
             svp::builder::ProgressEventKind::stage_started) == "stage_started");
  assert(svp::builder::progress_event_kind_name(
             svp::builder::ProgressEventKind::stage_completed) ==
             "stage_completed");
  assert(svp::builder::progress_event_kind_name(
             svp::builder::ProgressEventKind::stage_failed) == "stage_failed");
  assert(svp::builder::progress_event_kind_name(
             svp::builder::ProgressEventKind::warning) == "warning");
  assert(svp::builder::progress_event_kind_name(
             svp::builder::ProgressEventKind::artifact_written) ==
             "artifact_written");
}

void test_make_event_helpers() {
  const svp::builder::ProgressEvent started =
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::media_probe, "probing");
  assert(started.kind == svp::builder::ProgressEventKind::stage_started);
  assert(started.stage_id == svp::builder::ProgressStageId::media_probe);
  assert(started.message == "probing");

  const svp::builder::ProgressEvent completed =
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::media_probe);
  assert(completed.kind == svp::builder::ProgressEventKind::stage_completed);

  const svp::builder::ProgressEvent failed =
      svp::builder::make_stage_failed(
          svp::builder::ProgressStageId::validate, "validator exit 1");
  assert(failed.kind == svp::builder::ProgressEventKind::stage_failed);
  assert(failed.message == "validator exit 1");

  const svp::builder::ProgressEvent warning =
      svp::builder::make_warning(
          svp::builder::ProgressStageId::diarization, "fallback used");
  assert(warning.kind == svp::builder::ProgressEventKind::warning);
  assert(warning.message == "fallback used");

  const svp::builder::ProgressEvent artifact =
      svp::builder::make_artifact_written(
          svp::builder::ProgressStageId::package_write,
          "out/video.svp", "package");
  assert(artifact.kind == svp::builder::ProgressEventKind::artifact_written);
  assert(artifact.artifact_path == std::filesystem::path("out/video.svp"));
  assert(artifact.message == "package");
}

void test_null_sink_is_no_op() {
  svp::builder::NullBuildProgressSink sink;
  sink.emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe));
  sink.emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::media_probe));
}

void test_default_progress_sink_returns_null() {
  std::shared_ptr<svp::builder::BuildProgressSink> sink =
      svp::builder::default_progress_sink();
  assert(sink != nullptr);
  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe));
}

void test_pipeline_with_capturing_sink_emits_ordered_events() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_progress_test";
  std::filesystem::create_directories(tmp_dir);
  const std::filesystem::path probe_path = write_minimal_probe_json(tmp_dir);
  const std::filesystem::path output_path = tmp_dir / "output.json";

  auto capturing_sink = std::make_shared<CapturingProgressSink>();

  svp::builder::BuildPipelineOptions options;
  options.source_path = "test_video.mp4";
  options.probe_json_path = probe_path.string();
  options.output_path = output_path;
  options.stop_after = svp::builder::BuildStage::media_ingest;
  options.progress_sink = capturing_sink;

  svp::builder::BuildPipeline pipeline;
  const svp::builder::BuildPipelineResult result = pipeline.run(options);

  assert(result.exit_code == 0);
  assert(!capturing_sink->events.empty());

  const auto& events = capturing_sink->events;
  assert(events.size() >= 2);
  assert(events[0].kind ==
         svp::builder::ProgressEventKind::stage_started);
  assert(events[0].stage_id ==
         svp::builder::ProgressStageId::media_probe);
  assert(events[1].kind ==
         svp::builder::ProgressEventKind::stage_completed);
  assert(events[1].stage_id ==
         svp::builder::ProgressStageId::media_probe);

  bool found_artifact = false;
  for (const auto& event : events) {
    if (event.kind ==
        svp::builder::ProgressEventKind::artifact_written) {
      found_artifact = true;
      assert(event.artifact_path == output_path);
      break;
    }
  }
  assert(found_artifact);

  std::filesystem::remove_all(tmp_dir);
}

void test_pipeline_with_default_sink_preserves_behavior() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_progress_default_test";
  std::filesystem::create_directories(tmp_dir);
  const std::filesystem::path probe_path = write_minimal_probe_json(tmp_dir);
  const std::filesystem::path output_path = tmp_dir / "output.json";

  svp::builder::BuildPipelineOptions options;
  options.source_path = "test_video.mp4";
  options.probe_json_path = probe_path.string();
  options.output_path = output_path;
  options.stop_after = svp::builder::BuildStage::media_ingest;

  svp::builder::BuildPipeline pipeline;
  const svp::builder::BuildPipelineResult result = pipeline.run(options);

  assert(result.exit_code == 0);
  assert(std::filesystem::exists(output_path));

  std::filesystem::remove_all(tmp_dir);
}

}  // namespace

int main() {
  test_supported_stage_names_are_stable_and_ordered();
  test_stage_name_round_trip();
  test_execution_plan_preserves_existing_stage_conditions();
  test_default_staging_dir_matches_existing_cli_contract();
  test_package_skeleton_output_path_resolution();

  test_stage_catalog_ids_and_labels_from_one_source();
  test_event_kind_names();
  test_make_event_helpers();
  test_null_sink_is_no_op();
  test_default_progress_sink_returns_null();
  test_pipeline_with_capturing_sink_emits_ordered_events();
  test_pipeline_with_default_sink_preserves_behavior();

  return 0;
}
