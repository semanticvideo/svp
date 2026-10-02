#include "engine/canonical_frames_codec.hpp"
#include "engine/frame_catalog_delta.hpp"
#include "engine/ocr_frame_batch_plan.hpp"
#include "engine/stage_tasks.hpp"
#include "engine/vision_lane_settings.hpp"
#include "engine/vision_task_states.hpp"
#include "vision_stage_progress.hpp"

#include "svp/package/vision_lane_stages.hpp"
#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::builder::engine {
namespace {

svp::package::VisionLaneSettings vision_settings(const StageTaskEnvironment& environment) {
  return make_vision_lane_settings(environment.options, environment.plan, environment.plan_json,
                                   environment.staging_dir, environment.thread_plan,
                                   environment.model_runtime_available);
}

svp::vision::DecodedCanonicalFrames committed_canonical_frames(
    const StageTaskEnvironment& environment) {
  const std::string_view task_id = stage_task_id(StageTaskKind::canonical_frames);
  return decode_canonical_frames_state(
      environment.results.json_state(task_id, vision_state::kCanonicalFramesIndex),
      environment.results.state(task_id, vision_state::kCanonicalFramePixels));
}

// The OCR stage's reduce step over committed frame-batch results, in batch
// (sample-ordinal) order.
svp::package::VisionOcrStageResult reduce_ocr_frame_batches(
    const StageTaskEnvironment& environment, const OcrFrameBatchPlan& batches,
    StageTaskContext& task, const svp::package::SpatialProgressCallback& on_progress) {
  if (!environment.pp_ocr_sessions) {
    throw std::logic_error("OCR frame batches planned without a PP-OCR session pool");
  }
  std::vector<std::vector<svp::vision::OcrSampleDetections>> results;
  results.reserve(batches.nodes.size());
  for (const svp::exec::TaskNode& node : batches.nodes) {
    const std::vector<std::byte> bytes = environment.results.task_output(node.spec.task_id);
    results.push_back(svp::vision::tasks::read_ocr_frame_batch_output(
        node.spec, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
  }
  // OCR could not start on this Mac for some batch (ocr_frame_batch_task.hpp,
  // record_start_failures): run the stage exactly as a build without batches
  // does, so the package and its blocker are the ones that build writes.
  const bool started = std::none_of(results.begin(), results.end(), [](const auto& batch) {
    return std::any_of(batch.begin(), batch.end(), [](const auto& sample) {
      return sample.status == svp::vision::OcrSampleStatus::not_started;
    });
  });
  if (!started) {
    environment.pp_ocr_sessions->clear_idle();
    return svp::package::run_vision_ocr_stage(vision_settings(environment),
                                              committed_canonical_frames(environment),
                                              &task.context().frame_catalog, on_progress);
  }
  svp::package::VisionOcrStageResult reduced;
  {
    const auto lease = environment.pp_ocr_sessions->acquire(batches.work.pp_ocr);
    reduced = svp::package::run_vision_ocr_reduce_stage(
        vision_settings(environment), batches.work.samples, std::move(results),
        lease->session(), batches.work.pp_ocr, &task.context().frame_catalog, on_progress);
  }
  // Every batch has committed, so no session in the pool is needed again.
  environment.pp_ocr_sessions->clear_idle();
  return reduced;
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
  const svp::package::VisionOcrStageResult ocr =
      environment.ocr_batches != nullptr
          ? reduce_ocr_frame_batches(environment, *environment.ocr_batches, task,
                                     progress.callback())
          : svp::package::run_vision_ocr_stage(vision_settings(environment),
                                               committed_canonical_frames(environment),
                                               &task.context().frame_catalog,
                                               progress.callback());
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
