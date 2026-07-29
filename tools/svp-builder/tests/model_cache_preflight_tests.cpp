#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/model_cache_preflight.hpp"
#include "model_cache_test_fixture.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct TemporaryDirectory {
  fs::path path;

  TemporaryDirectory() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("svp-builder-model-preflight-tests-" + std::to_string(unique));
    fs::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};

class CapturingProgressSink : public svp::builder::BuildProgressSink {
 public:
  void emit(const svp::builder::ProgressEvent& event) override {
    events.push_back(event);
  }

  std::vector<svp::builder::ProgressEvent> events;
};

void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

nlohmann::json read_json(const fs::path& path) {
  std::ifstream input(path);
  nlohmann::json value;
  input >> value;
  return value;
}

void write_probe_json(const fs::path& path) {
  svp::builder::test::write_text(path, R"({
    "format_name": "mov,mp4,m4a,3gp,3g2,mj2",
    "container_timing": {
      "timebase": "1/1000000",
      "start_pts": 0,
      "duration_pts": 1000000
    },
    "video_streams": [{
      "id": "vstream_0001",
      "index": 0,
      "codec_name": "h264",
      "width": 16,
      "height": 16,
      "pixel_aspect_ratio": "1:1",
      "timing": {
        "timebase": "1/30",
        "start_pts": 0,
        "duration_pts": 30,
        "avg_frame_rate": "30/1",
        "frame_count": 30
      }
    }],
    "audio_streams": []
  })");
}

std::string expect_preflight_failure(const fs::path& cache_root) {
  try {
    svp::builder::verify_authoritative_model_cache(cache_root);
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    expect(message.find(cache_root.string()) != std::string::npos,
           "preflight failure did not identify the model-cache path");
    expect(message.find("authoritative model-cache verification failed") !=
               std::string::npos,
           "preflight failure did not identify authoritative verification");
    return message;
  }
  throw std::runtime_error("invalid model cache passed preflight");
}

void test_valid_cache_passes() {
  TemporaryDirectory temporary;
  svp::builder::test::write_valid_model_cache(temporary.path);
  svp::builder::verify_authoritative_model_cache(temporary.path);
}

void test_missing_and_invalid_root_lock_fail() {
  TemporaryDirectory missing;
  fs::create_directories(missing.path / "bundle");
  const std::string missing_error = expect_preflight_failure(missing.path);
  expect(missing_error.find("required root model lock is missing") !=
             std::string::npos,
         "missing root lock did not produce the required diagnostic");

  TemporaryDirectory invalid;
  svp::builder::test::write_valid_model_cache(invalid.path);
  svp::builder::test::write_text(invalid.path / "model-lock.json", "not json\n");
  const std::string invalid_error = expect_preflight_failure(invalid.path);
  expect(invalid_error.find("model-lock.json") != std::string::npos,
         "invalid root lock diagnostic did not identify model-lock.json");
}

void test_lock_manifest_disagreement_fails() {
  TemporaryDirectory temporary;
  svp::builder::test::write_valid_model_cache(temporary.path);
  nlohmann::json lock = read_json(temporary.path / "model-lock.json");
  lock["models"][0]["files"][0]["role"] = "weights";
  svp::builder::test::write_text(temporary.path / "model-lock.json",
                                 lock.dump(2) + "\n");
  const std::string error = expect_preflight_failure(temporary.path);
  expect(error.find("file identity disagrees") != std::string::npos,
         "lock/manifest disagreement was not reported");
}

void test_changed_model_file_fails() {
  TemporaryDirectory temporary;
  svp::builder::test::write_valid_model_cache(temporary.path);
  svp::builder::test::write_text(temporary.path / "bundle" / "model.onnx",
                                 "changed model bytes\n");
  const std::string error = expect_preflight_failure(temporary.path);
  expect(error.find("BLAKE3 mismatch") != std::string::npos,
         "changed model file was not rejected");
  expect(error.find("bundle BLAKE3 mismatch") != std::string::npos,
         "changed model file did not invalidate the aggregate bundle digest");
}

void test_changed_legal_files_fail_bundle_digest() {
  for (std::string_view filename : {"LICENSE", "NOTICE"}) {
    TemporaryDirectory temporary;
    svp::builder::test::write_valid_model_cache(temporary.path);
    svp::builder::test::write_text(temporary.path / "bundle" / filename,
                                   "changed legal text\n");
    const std::string error = expect_preflight_failure(temporary.path);
    expect(error.find("bundle BLAKE3 mismatch") != std::string::npos,
           "changed LICENSE or NOTICE did not invalidate bundle_blake3");
  }
}

void test_missing_unexpected_and_duplicate_bundles_fail() {
  TemporaryDirectory missing;
  svp::builder::test::write_valid_model_cache(missing.path);
  fs::remove_all(missing.path / "bundle");
  const std::string missing_error = expect_preflight_failure(missing.path);
  expect(missing_error.find("missing model bundle") != std::string::npos,
         "missing bundle was not reported");

  TemporaryDirectory unexpected;
  svp::builder::test::write_valid_model_cache(unexpected.path);
  svp::builder::test::write_valid_model_bundle(
      unexpected.path / "extra", "model_builder_unexpected_test", "1.0");
  const std::string unexpected_error = expect_preflight_failure(unexpected.path);
  expect(unexpected_error.find("unexpected model bundle") != std::string::npos,
         "unexpected bundle was not reported");

  TemporaryDirectory duplicate;
  svp::builder::test::write_valid_model_cache(duplicate.path);
  fs::copy(duplicate.path / "bundle", duplicate.path / "duplicate",
           fs::copy_options::recursive);
  const std::string duplicate_error = expect_preflight_failure(duplicate.path);
  expect(duplicate_error.find("duplicate installed model_bundle_id") !=
             std::string::npos,
         "duplicate bundle identity was not reported");
}

void test_execution_plan_model_backed_boundary() {
  using svp::builder::BuildStage;
  using svp::builder::execution_plan_for_stage;
  using svp::builder::schedules_model_backed_work;

  expect(!schedules_model_backed_work(
             execution_plan_for_stage(BuildStage::media_ingest)),
         "media ingest unexpectedly requires model-cache verification");
  expect(schedules_model_backed_work(execution_plan_for_stage(BuildStage::audio)),
         "audio did not require model-cache verification");
  expect(!schedules_model_backed_work(
             execution_plan_for_stage(BuildStage::vision_plan)),
         "vision planning unexpectedly requires model-cache verification");
  expect(!schedules_model_backed_work(
             execution_plan_for_stage(BuildStage::foundation_color)),
         "foundation color unexpectedly requires model-cache verification");
  expect(schedules_model_backed_work(
             execution_plan_for_stage(BuildStage::foundation_ocr)),
         "foundation OCR did not require model-cache verification");
  expect(schedules_model_backed_work(
             execution_plan_for_stage(BuildStage::package_skeleton)),
         "package build did not require model-cache verification");
}

void test_failure_precedes_media_probe_and_ffprobe() {
  TemporaryDirectory temporary;
  const fs::path cache_root = temporary.path / "cache";
  svp::builder::test::write_valid_model_cache(cache_root);
  svp::builder::test::write_text(cache_root / "bundle" / "model.onnx",
                                 "changed model bytes\n");

  const fs::path marker = temporary.path / "ffprobe-called";
  const fs::path ffprobe = temporary.path / "ffprobe-marker.sh";
  svp::builder::test::write_text(
      ffprobe, "#!/bin/sh\n: > \"" + marker.string() + "\"\nexit 1\n");
  fs::permissions(ffprobe, fs::perms::owner_read | fs::perms::owner_write |
                               fs::perms::owner_exec);

  auto sink = std::make_shared<CapturingProgressSink>();
  svp::builder::BuildPipelineOptions options;
  options.source_path = (temporary.path / "source.mp4").string();
  options.ffprobe_path = ffprobe.string();
  options.output_path = temporary.path / "output.json";
  options.model_cache_dir = cache_root;
  options.stop_after = svp::builder::BuildStage::audio;
  options.progress_sink = sink;

  std::ostringstream captured_stderr;
  std::streambuf* previous = std::cerr.rdbuf(captured_stderr.rdbuf());
  const svp::builder::BuildPipelineResult result =
      svp::builder::BuildPipeline{}.run(options);
  std::cerr.rdbuf(previous);

  expect(result.exit_code != 0, "invalid cache did not stop the build");
  expect(sink->events.empty(),
         "media-processing progress was emitted before preflight failure");
  expect(!fs::exists(marker), "ffprobe ran before preflight failure");
  expect(captured_stderr.str().find("bundle BLAKE3 mismatch") !=
             std::string::npos,
         "pipeline failure omitted verifier diagnostics");
}

void test_successful_preflight_continues_existing_build_path() {
  TemporaryDirectory temporary;
  const fs::path cache_root =
      svp::builder::test::write_valid_model_cache(temporary.path / "cache");
  const fs::path probe_path = temporary.path / "probe.json";
  write_probe_json(probe_path);

  auto sink = std::make_shared<CapturingProgressSink>();
  svp::builder::BuildPipelineOptions options;
  options.source_path = (temporary.path / "source.mp4").string();
  options.probe_json_path = probe_path.string();
  options.ffmpeg_path = "/usr/bin/false";
  options.output_path = temporary.path / "output.json";
  options.staging_dir = temporary.path / "staging";
  options.model_cache_dir = cache_root;
  options.stop_after = svp::builder::BuildStage::audio;
  options.progress_sink = sink;

  static_cast<void>(svp::builder::BuildPipeline{}.run(options));
  expect(!sink->events.empty(),
         "valid preflight did not allow the build path to continue");
  expect(sink->events.front().kind ==
             svp::builder::ProgressEventKind::stage_started &&
             sink->events.front().stage_id ==
                 svp::builder::ProgressStageId::media_probe,
         "successful preflight did not continue at media probing");
}

void test_non_model_build_remains_cache_independent() {
  TemporaryDirectory temporary;
  const fs::path probe_path = temporary.path / "probe.json";
  write_probe_json(probe_path);

  svp::builder::BuildPipelineOptions options;
  options.source_path = (temporary.path / "source.mp4").string();
  options.probe_json_path = probe_path.string();
  options.output_path = temporary.path / "output.json";
  options.model_cache_dir = temporary.path / "missing-cache";
  options.stop_after = svp::builder::BuildStage::media_ingest;

  const svp::builder::BuildPipelineResult result =
      svp::builder::BuildPipeline{}.run(options);
  expect(result.exit_code == 0,
         "non-model build was blocked by an absent model cache");
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2) {
    svp::builder::verify_authoritative_model_cache(argv[1]);
    std::cout << "svp-builder model cache preflight: PASS " << argv[1] << "\n";
    return 0;
  }
  test_valid_cache_passes();
  test_missing_and_invalid_root_lock_fail();
  test_lock_manifest_disagreement_fails();
  test_changed_model_file_fails();
  test_changed_legal_files_fail_bundle_digest();
  test_missing_unexpected_and_duplicate_bundles_fail();
  test_execution_plan_model_backed_boundary();
  test_failure_precedes_media_probe_and_ffprobe();
  test_successful_preflight_continues_existing_build_path();
  test_non_model_build_remains_cache_independent();
  std::cout << "svp-builder model cache preflight tests: PASS\n";
  return 0;
}
