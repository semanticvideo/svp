#include "engine/canonical_frames_codec.hpp"
#include "engine/frame_catalog_delta.hpp"
#include "engine/stage_tasks.hpp"
#include "engine/vision_task_states.hpp"
#include "vision_stage_progress.hpp"

#include "svp/package/vision_lane_stages.hpp"

namespace svp::builder::engine {
namespace {

svp::package::VisionLaneSettings vision_settings(const StageTaskEnvironment& environment) {
  return svp::package::VisionLaneSettings{
      .staging_dir = environment.staging_dir,
      .model_runtime_available = environment.model_runtime_available,
      .media_plan_json = environment.plan_json,
      .model_cache_root = environment.options.model_cache_dir,
      .media_plan = &environment.plan,
      .ffmpeg_path = environment.options.ffmpeg_path,
      .performance = environment.options.performance,
      .thread_plan = environment.thread_plan,
      .visual_tracking_quality = environment.options.visual_tracking_quality,
  };
}

svp::vision::DecodedCanonicalFrames committed_canonical_frames(
    const StageTaskEnvironment& environment) {
  const std::string_view task_id = stage_task_id(StageTaskKind::canonical_frames);
  return decode_canonical_frames_state(
      environment.results.json_state(task_id, vision_state::kCanonicalFramesIndex),
      environment.results.state(task_id, vision_state::kCanonicalFramePixels));
}

void add_catalog_delta(StageStates& states, StageTaskContext& task) {
  states[state_name::kFrameCatalog] =
      json_state_bytes(frame_catalog_delta(task.context().frame_catalog));
}

}  // namespace

StageStates run_canonical_frames_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  const svp::vision::DecodedCanonicalFrames frames =
      svp::package::decode_vision_lane_canonical_frames(vision_settings(environment),
                                                        &task.context().frame_catalog);
  EncodedCanonicalFrames encoded = encode_canonical_frames(frames);
  StageStates states;
  states[vision_state::kCanonicalFramesIndex] = json_state_bytes(encoded.index);
  states[vision_state::kCanonicalFramePixels] = std::move(encoded.pixels);
  add_catalog_delta(states, task);
  return states;
}

StageStates run_depth_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  VisionStageProgress progress(task.context());
  const svp::package::VisionDepthStageResult depth = svp::package::run_vision_depth_stage(
      vision_settings(environment), committed_canonical_frames(environment),
      progress.callback());
  progress.finish();
  StageStates states;
  states[vision_state::kDepth] = json_state_bytes(svp::package::to_json(depth));
  return states;
}

StageStates run_ocr_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  VisionStageProgress progress(task.context());
  const svp::package::VisionOcrStageResult ocr = svp::package::run_vision_ocr_stage(
      vision_settings(environment), committed_canonical_frames(environment),
      &task.context().frame_catalog, progress.callback());
  progress.finish();
  StageStates states;
  states[vision_state::kOcr] = json_state_bytes(svp::package::to_json(ocr));
  add_catalog_delta(states, task);
  return states;
}

StageStates run_text_embeddings_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  VisionStageProgress progress(task.context());
  const svp::package::VisionTextEmbeddingStageResult embeddings =
      svp::package::run_vision_text_embedding_stage(vision_settings(environment),
                                                    progress.callback());
  progress.finish();
  StageStates states;
  states[vision_state::kTextEmbeddings] =
      json_state_bytes(svp::package::to_json(embeddings));
  return states;
}

StageStates run_tracking_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  VisionStageProgress progress(task.context());
  const svp::package::VisionTrackingStageResult tracking =
      svp::package::run_vision_tracking_stage(vision_settings(environment),
                                              &task.context().frame_catalog,
                                              progress.callback());
  progress.finish();
  StageStates states;
  states[vision_state::kTracking] = json_state_bytes(svp::package::to_json(tracking));
  add_catalog_delta(states, task);
  return states;
}

}  // namespace svp::builder::engine
