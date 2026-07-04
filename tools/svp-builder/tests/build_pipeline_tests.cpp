#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_progress.hpp"
#include "svp/builder/progress_renderer.hpp"
#include "staging_cleanup.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
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
                       "package",
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
  assert(!svp::builder::parse_build_stage("foundation-audio"));
}

void test_package_skeleton_alias_is_temporarily_supported() {
  const std::optional<svp::builder::BuildStage> canonical =
      svp::builder::parse_build_stage("package");
  const std::optional<svp::builder::BuildStage> alias =
      svp::builder::parse_build_stage("package-skeleton");

  assert(canonical.has_value());
  assert(alias.has_value());
  assert(*canonical == *alias);
  assert(svp::builder::build_stage_name(*alias) == "package");
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

  assert(svp::builder::progress_stage_id(
             svp::builder::ProgressStageId::ocr_evidence_crops) ==
         "ocr_evidence_crops");
  assert(svp::builder::progress_stage_label(
             svp::builder::ProgressStageId::ocr_evidence_crops) ==
         "OCR Evidence Crops");
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
             svp::builder::ProgressEventKind::stage_progress) ==
             "stage_progress");
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

  const svp::builder::ProgressEvent progress =
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::asr, 3, 10, "chunks");
  assert(progress.kind == svp::builder::ProgressEventKind::stage_progress);
  assert(progress.stage_id == svp::builder::ProgressStageId::asr);
  assert(progress.current.has_value());
  assert(progress.total.has_value());
  assert(progress.fraction.has_value());
  assert(*progress.current == 3);
  assert(*progress.total == 10);
  assert(*progress.fraction == 0.3);
  assert(progress.unit == "chunks");
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

  bool found_package_write = false;
  bool found_validate = false;
  for (const auto& event : events) {
    if (event.stage_id == svp::builder::ProgressStageId::package_write)
      found_package_write = true;
    if (event.stage_id == svp::builder::ProgressStageId::validate)
      found_validate = true;
  }
  assert(!found_package_write);
  assert(!found_validate);

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

void test_pipeline_package_write_failure_emits_stage_failed_no_validate() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_progress_pkg_fail";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);
  const std::filesystem::path probe_path = write_minimal_probe_json(tmp_dir);
  const std::filesystem::path staging_dir = tmp_dir / "staging";

  const std::filesystem::path pkg_blocker = tmp_dir / "output.svp";
  std::filesystem::create_directories(pkg_blocker);

  const std::filesystem::path output_path = tmp_dir / "output.json";

  auto capturing_sink = std::make_shared<CapturingProgressSink>();

  svp::builder::BuildPipelineOptions options;
  options.source_path = "test_video.mp4";
  options.probe_json_path = probe_path.string();
  options.output_path = output_path;
  options.staging_dir = staging_dir;
  options.stop_after = svp::builder::BuildStage::package_skeleton;
  options.force_single_speaker = true;
  options.allow_fallback_diarization = true;
  options.progress_sink = capturing_sink;

  svp::builder::BuildPipeline pipeline;
  const svp::builder::BuildPipelineResult result = pipeline.run(options);

  bool found_pkg_started = false;
  bool found_pkg_failed = false;
  bool found_pkg_completed = false;
  bool found_validate = false;
  for (const auto& event : capturing_sink->events) {
    if (event.stage_id == svp::builder::ProgressStageId::package_write) {
      if (event.kind == svp::builder::ProgressEventKind::stage_started)
        found_pkg_started = true;
      if (event.kind == svp::builder::ProgressEventKind::stage_failed)
        found_pkg_failed = true;
      if (event.kind == svp::builder::ProgressEventKind::stage_completed)
        found_pkg_completed = true;
    }
    if (event.stage_id == svp::builder::ProgressStageId::validate)
      found_validate = true;
  }

  assert(found_pkg_started);
  assert(found_pkg_failed);
  assert(!found_pkg_completed);
  assert(!found_validate);

  std::filesystem::remove_all(tmp_dir);
}

void test_warning_event_factory_for_diarization_and_index() {
  const svp::builder::ProgressEvent diarization_warning =
      svp::builder::make_warning(
          svp::builder::ProgressStageId::diarization,
          "Fallback diarization active; sherpa-onnx not available.");
  assert(diarization_warning.kind == svp::builder::ProgressEventKind::warning);
  assert(diarization_warning.stage_id ==
         svp::builder::ProgressStageId::diarization);
  assert(diarization_warning.message ==
         "Fallback diarization active; sherpa-onnx not available.");

  const svp::builder::ProgressEvent index_warning =
      svp::builder::make_warning(
          svp::builder::ProgressStageId::index,
          "Failed to write SQLite index foundation.");
  assert(index_warning.kind == svp::builder::ProgressEventKind::warning);
  assert(index_warning.stage_id ==
         svp::builder::ProgressStageId::index);
  assert(index_warning.message ==
         "Failed to write SQLite index foundation.");
}

void test_stage_progress_event_has_progress_fields() {
  const svp::builder::ProgressEvent event =
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::color, 7, 15, "frames", "sampling");
  assert(event.kind == svp::builder::ProgressEventKind::stage_progress);
  assert(event.current.has_value());
  assert(event.total.has_value());
  assert(event.fraction.has_value());
  assert(*event.current == 7);
  assert(*event.total == 15);
  assert(event.unit == "frames");
  assert(event.message == "sampling");
}

void test_stage_progress_zero_total_no_fraction() {
  const svp::builder::ProgressEvent event =
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::asr, 0, 0, "chunks");
  assert(event.current.has_value());
  assert(event.total.has_value());
  assert(!event.fraction.has_value());
}

void test_pipeline_quiet_suppresses_summary() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_quiet_test";
  std::filesystem::remove_all(tmp_dir);
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
  options.quiet = true;

  std::ostringstream captured_stdout;
  std::streambuf* old_cout = std::cout.rdbuf();
  std::cout.rdbuf(captured_stdout.rdbuf());

  svp::builder::BuildPipeline pipeline;
  const svp::builder::BuildPipelineResult result = pipeline.run(options);

  std::cout.rdbuf(old_cout);

  assert(result.exit_code == 0);
  assert(std::filesystem::exists(output_path));
  assert(captured_stdout.str().empty());

  std::filesystem::remove_all(tmp_dir);
}

void test_pipeline_verbose_emits_events() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_verbose_test";
  std::filesystem::remove_all(tmp_dir);
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
  options.verbose = true;

  std::ostringstream captured_stdout;
  std::streambuf* old_cout = std::cout.rdbuf();
  std::cout.rdbuf(captured_stdout.rdbuf());

  svp::builder::BuildPipeline pipeline;
  const svp::builder::BuildPipelineResult result = pipeline.run(options);

  std::cout.rdbuf(old_cout);

  assert(result.exit_code == 0);
  assert(!capturing_sink->events.empty());

  std::filesystem::remove_all(tmp_dir);
}

void test_plain_renderer_no_raw_event_names() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe, "probing"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::media_probe));
  sink->emit(svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::asr, 3, 10, "chunks"));

  const std::string output = oss.str();
  assert(output.find("[stage_started]") == std::string::npos);
  assert(output.find("[stage_completed]") == std::string::npos);
  assert(output.find("[stage_progress]") == std::string::npos);
  assert(output.find("Media Probe") != std::string::npos);
  assert(output.find("ASR") != std::string::npos);
}

void test_plain_renderer_no_ffmpeg_noise_strings() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::audio_extract, "extracting"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::audio_extract));

  const std::string output = oss.str();
  assert(output.find("Input #0") == std::string::npos);
  assert(output.find("Stream mapping:") == std::string::npos);
  assert(output.find("Schema error") == std::string::npos);
}

void test_plain_renderer_no_noise_categories() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe, "probing"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::media_probe));
  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::asr, "transcribing"));
  sink->emit(svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::asr, 1, 3, "chunks"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::asr));
  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::color, "sampling"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::color));

  const std::string output = oss.str();

  assert(output.find("Schema error") == std::string::npos);
  assert(output.find("Trying to register schema") == std::string::npos);
  assert(output.find("Debug (cpuinfo)") == std::string::npos);
  assert(output.find("Note (cpuinfo)") == std::string::npos);
  assert(output.find("ParallelBackendRegistry") == std::string::npos);
  assert(output.find("OpenCV(") == std::string::npos);
  assert(output.find("Input #0") == std::string::npos);
  assert(output.find("Stream mapping:") == std::string::npos);
  assert(output.find("[stage_started]") == std::string::npos);
  assert(output.find("[stage_completed]") == std::string::npos);
  assert(output.find("[stage_progress]") == std::string::npos);
}

void test_json_sink_no_noise_categories() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::asr, "transcribing"));
  sink->emit(svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::asr, 1, 3, "chunks"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::asr));

  const std::string output = oss.str();

  assert(output.find("Schema error") == std::string::npos);
  assert(output.find("Trying to register schema") == std::string::npos);
  assert(output.find("Debug (cpuinfo)") == std::string::npos);
  assert(output.find("Note (cpuinfo)") == std::string::npos);
  assert(output.find("ParallelBackendRegistry") == std::string::npos);
  assert(output.find("OpenCV(") == std::string::npos);
  assert(output.find("Input #0") == std::string::npos);
  assert(output.find("Stream mapping:") == std::string::npos);
  assert(output.find("[stage_started]") == std::string::npos);
}

void test_quiet_produces_no_stdout() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_quiet_stdout_test";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);
  const std::filesystem::path probe_path = write_minimal_probe_json(tmp_dir);
  const std::filesystem::path output_path = tmp_dir / "output.json";

  svp::builder::BuildPipelineOptions options;
  options.source_path = "test_video.mp4";
  options.probe_json_path = probe_path.string();
  options.output_path = output_path;
  options.stop_after = svp::builder::BuildStage::media_ingest;
  options.quiet = true;

  std::ostringstream captured_stdout;
  std::streambuf* old_cout = std::cout.rdbuf();
  std::cout.rdbuf(captured_stdout.rdbuf());

  svp::builder::BuildPipeline pipeline;
  const svp::builder::BuildPipelineResult result = pipeline.run(options);

  std::cout.rdbuf(old_cout);

  assert(result.exit_code == 0);
  assert(captured_stdout.str().empty());
  assert(std::filesystem::exists(output_path));

  std::filesystem::remove_all(tmp_dir);
}

void test_noise_regression_forbidden_strings_absent() {
  // Render a full set of events through plain mode and verify
  // no forbidden noise strings appear in the output.
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, oss, false);

  const svp::builder::ProgressEvent events[] = {
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::media_probe, "probing"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::media_probe),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::audio_extract, "extracting"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::audio_extract),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::asr, "transcribing"),
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::asr, 1, 3, "chunks"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::asr),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::diarization),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::diarization),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::color, "sampling"),
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::color, 5, 15, "frames"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::color),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::ocr, "detecting"),
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::ocr, 10, 34, "items"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::ocr),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::ocr_evidence_crops),
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::ocr_evidence_crops, 10, 20, "steps"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::ocr_evidence_crops),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::depth),
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::depth, 3, 5, "items"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::depth),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::text_embeddings),
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::text_embeddings, 10, 23, "items"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::text_embeddings),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::visual_tracking),
      svp::builder::make_stage_progress(
          svp::builder::ProgressStageId::visual_tracking, 1, 1, "items"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::visual_tracking),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::entities),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::entities),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::relationships),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::relationships),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::index),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::index),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::package_write),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::package_write),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::validate),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::validate),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::validation_report),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::validation_report),
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::repackage),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::repackage),
      svp::builder::make_artifact_written(
          svp::builder::ProgressStageId::package_write,
          "out/video.svp", "package"),
  };

  for (const auto& event : events) {
    sink->emit(event);
  }

  const std::string output = oss.str();

  // Forbidden strings must not appear
  const std::string forbidden[] = {
      "Schema error",
      "Trying to register schema",
      "ffmpeg version",
      "Input #0",
      "Output #0",
      "Wrote:",
      "Validation: PASSED",
      "+ Package Write:",
      "[stage_started]",
      "[stage_progress]",
      "[stage_completed]",
      "OpenCV(",
      "onnxruntime",
  };

  for (const auto& s : forbidden) {
    assert(output.find(s) == std::string::npos);
  }

  // Required stage labels must appear
  assert(output.find("Media Probe") != std::string::npos);
  assert(output.find("Audio Extraction") != std::string::npos);
  assert(output.find("ASR") != std::string::npos);
  assert(output.find("Diarization") != std::string::npos);
  assert(output.find("Color Observations") != std::string::npos);
  assert(output.find("OCR") != std::string::npos);
  assert(output.find("OCR Evidence Crops") != std::string::npos);
  assert(output.find("Depth") != std::string::npos);
  assert(output.find("Text Embeddings") != std::string::npos);
  assert(output.find("Visual Tracking") != std::string::npos);
  assert(output.find("Entities") != std::string::npos);
  assert(output.find("Relationships") != std::string::npos);
  assert(output.find("Index") != std::string::npos);
  assert(output.find("Package Write") != std::string::npos);
  assert(output.find("Validation Report") != std::string::npos);
  assert(output.find("Repackage") != std::string::npos);
  assert(output.find("Validation") != std::string::npos);
}

void test_json_progress_preserves_all_event_types() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::ocr, "detecting"));
  sink->emit(svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::ocr, 10, 34, "items"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::ocr));
  sink->emit(svp::builder::make_artifact_written(
      svp::builder::ProgressStageId::package_write,
      "out/video.svp", "package"));

  const std::string output = oss.str();

  // JSON mode must preserve artifact_written (suppressed in human modes)
  assert(output.find("\"artifact_written\"") != std::string::npos);
  assert(output.find("\"stage_started\"") != std::string::npos);
  assert(output.find("\"stage_progress\"") != std::string::npos);
  assert(output.find("\"stage_completed\"") != std::string::npos);
  assert(output.find("\"ocr\"") != std::string::npos);
  assert(output.find("\"items\"") != std::string::npos);
}

void test_default_staging_removed_after_success() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_staging_cleanup_default";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  svp::builder::StagingCleanupGuard guard(tmp_dir, false);
  guard.cleanup_on_success();

  assert(!std::filesystem::exists(tmp_dir));
}

void test_explicit_staging_preserved_after_success() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_staging_cleanup_explicit";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  svp::builder::StagingCleanupGuard guard(tmp_dir, true);
  guard.cleanup_on_success();

  assert(std::filesystem::exists(tmp_dir));
  std::filesystem::remove_all(tmp_dir);
}

void test_default_staging_preserved_on_failure() {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "svp_staging_cleanup_fail";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  {
    svp::builder::StagingCleanupGuard guard(tmp_dir, false);
  }

  assert(std::filesystem::exists(tmp_dir));
  std::filesystem::remove_all(tmp_dir);
}

}  // namespace

int main() {
  test_supported_stage_names_are_stable_and_ordered();
  test_stage_name_round_trip();
  test_package_skeleton_alias_is_temporarily_supported();
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
  test_pipeline_package_write_failure_emits_stage_failed_no_validate();
  test_warning_event_factory_for_diarization_and_index();
  test_stage_progress_event_has_progress_fields();
  test_stage_progress_zero_total_no_fraction();
  test_pipeline_quiet_suppresses_summary();
  test_pipeline_verbose_emits_events();
  test_plain_renderer_no_raw_event_names();
  test_plain_renderer_no_ffmpeg_noise_strings();
  test_plain_renderer_no_noise_categories();
  test_json_sink_no_noise_categories();
  test_quiet_produces_no_stdout();
  test_noise_regression_forbidden_strings_absent();
  test_json_progress_preserves_all_event_types();
  test_default_staging_removed_after_success();
  test_explicit_staging_preserved_after_success();
  test_default_staging_preserved_on_failure();

  return 0;
}
