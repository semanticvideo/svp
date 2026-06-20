#include "svp/vision/observation_pipeline_plan.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/rational.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

svp::media::MediaIngestPlan sample_media_plan() {
  svp::media::MediaProbe probe;
  svp::media::VideoStreamProbe video_stream;
  video_stream.id = "v:0";
  video_stream.index = 0;
  video_stream.codec_name = "h264";
  video_stream.width = 1920;
  video_stream.height = 1080;
  video_stream.rotation_degrees = 0;
  video_stream.pixel_aspect_ratio = svp::media::Rational{1, 1};
  video_stream.timing = svp::media::StreamTiming{};
  probe.video_streams.push_back(video_stream);

  return svp::media::build_media_ingest_plan("sample.mov", std::move(probe));
}

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    std::exit(1);
  }
}

bool has_task_boundary(const nlohmann::json& tasks, const std::string& boundary) {
  for (const nlohmann::json& task : tasks) {
    if (task.at("processor_boundary") == boundary) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  const svp::vision::VisionObservationPipelinePlan plan =
      svp::vision::build_vision_observation_pipeline_plan(sample_media_plan());
  const nlohmann::json json =
      svp::vision::vision_observation_pipeline_plan_to_json(plan);

  require(json.at("observations_generated") == false,
          "foundation plan must not claim generated observations");
  require(json.at("model_execution_performed") == false,
          "foundation plan must not claim model execution");
  require(json.at("valid_svp_package_written") == false,
          "foundation plan must not claim package writing");

  const nlohmann::json& tasks = json.at("tasks");
  require(has_task_boundary(tasks, "ocr_text_detection"),
          "missing OCR detection task boundary");
  require(has_task_boundary(tasks, "ocr_text_recognition"),
          "missing OCR recognition task boundary");
  require(has_task_boundary(tasks, "ocr_numeric_extraction"),
          "missing numeric extraction task boundary");
  require(has_task_boundary(tasks, "color_bucket_quantization"),
          "missing color quantization task boundary");
  require(has_task_boundary(tasks, "scene_color_summary"),
          "missing scene color summary task boundary");
  require(has_task_boundary(tasks, "shot_color_summary"),
          "missing shot color summary task boundary");
  require(has_task_boundary(tasks, "frame_color_summary"),
          "missing frame color summary task boundary");
  require(has_task_boundary(tasks, "region_color_summary"),
          "missing region color summary task boundary");
  require(has_task_boundary(tasks, "text_region_color_summary"),
          "missing text-region color summary task boundary");
  require(has_task_boundary(tasks, "text_region_foreground_background_color"),
          "missing text-region foreground/background color task boundary");
  require(has_task_boundary(tasks, "ocr_color_provenance_hooks"),
          "missing provenance hook task boundary");

  return 0;
}
