#include "default_build_calibration.hpp"

#include "coordinator_context.hpp"
#include "calibration/sherpa_library_naming.hpp"
#include "engine/distributed_audio_work.hpp"
#include "engine/distributed_vision_work.hpp"

#include "svp/package/vision_lane_stages.hpp"
#include "svp/vision/tasks/track_window_spec.hpp"

#include <algorithm>

namespace svp::builder::workers {
namespace {

void add_model(std::vector<std::string>& ids, const std::string& id) {
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
    ids.push_back(id);
  }
}

}  // namespace

DefaultBuildCalibration default_build_calibration(const std::filesystem::path& model_cache) {
  DefaultBuildCalibration work;
  work.ocr = default_ocr_calibration_setup(model_cache);
  const svp::models::ThreadPlan thread_plan = default_build_thread_plan(model_cache);
  for (const svp::exec::TaskModelRef& ref : work.ocr.model_refs) {
    add_model(work.model_ids, ref.model_id);
  }

  // The vision types (M4), as the build plans them.
  work.dispatched = calibration::DispatchedCalibrationSetup{
      .pp_ocr = work.ocr.pp_ocr,
      .pp_ocr_model_refs = work.ocr.model_refs,
      .vision = engine::plan_distributed_vision_work(model_cache, thread_plan),
      .ffmpeg_build = work.ocr.ffmpeg_build};
  for (const std::optional<DistributedOnnxWork>* onnx :
       {&work.dispatched.vision.text_embeddings, &work.dispatched.vision.keyframe_embeddings,
        &work.dispatched.vision.depth}) {
    if (*onnx) {
      add_model(work.model_ids, (*onnx)->model_ref.model_id);
    }
  }

  // Tracking windows (M4), at the qualities measured ahead of builds; each
  // keeps its own record.
  std::vector<svp::vision::VisualTrackingQuality> qualities;
  for (const svp::vision::VisualTrackingQuality quality :
       calibration::calibrated_tracking_qualities()) {
    svp::package::VisionLaneSettings settings;
    settings.model_cache_root = model_cache;
    settings.ffmpeg_path = work.ocr.ffmpeg_path;
    settings.thread_plan = thread_plan;
    settings.visual_tracking_quality = std::string(svp::vision::visual_tracking_quality_name(quality));
    TrackWindowCalibrationSetup setup{.options = svp::package::make_vision_tracking_options(settings),
                                      .model_refs = {},
                                      .model_cache_root = model_cache,
                                      .ffmpeg_path = work.ocr.ffmpeg_path,
                                      .ffmpeg_build = work.ocr.ffmpeg_build};
    try {
      setup.model_refs = svp::vision::tasks::track_window_model_refs(model_cache, setup.options);
    } catch (const std::exception& error) {
      work.skipped.push_back("track.window (" +
                             std::string(svp::vision::visual_tracking_quality_name(quality)) +
                             "): " + error.what());
      continue;
    }
    for (const svp::exec::TaskModelRef& ref : setup.model_refs) {
      add_model(work.model_ids, ref.model_id);
    }
    work.tracking.emplace(quality, std::move(setup));
    qualities.push_back(quality);
  }

  // The audio types (M5), as a build with audio plans them.
  work.audio = calibration::AudioCalibrationSetup{
      .audio = engine::plan_distributed_audio_models(model_cache, thread_plan),
      .thread_plan = thread_plan,
      .sherpa_library = {}};
  // The library diarize.window is measured against, found as a build finds
  // it; a library problem skips that type only.
  if (const std::optional<std::string> skipped =
          calibration::name_diarization_library(work.audio, current_executable())) {
    work.skipped.push_back(*skipped);
  }
  for (const svp::exec::TaskModelRef& ref : work.audio.audio.asr_model_refs) {
    add_model(work.model_ids, ref.model_id);
  }
  if (work.audio.audio.diarization_model_ref) {
    add_model(work.model_ids, work.audio.audio.diarization_model_ref->model_id);
  }

  work.steps = calibration::calibration_steps(work.dispatched.vision, work.audio.audio, qualities);
  return work;
}

}  // namespace svp::builder::workers
