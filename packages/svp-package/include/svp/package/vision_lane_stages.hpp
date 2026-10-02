#pragma once

// The vision lane of a package build, one function per stage, so each stage
// can run as its own task (RC2 §5.16, §20.1-§20.2) and a resumed build can
// skip the stages it already finished.
//
// Composing the stages in this order reproduces
// write_spatial_and_embedding_placeholders exactly (that function is built
// from them):
//
//   decode_vision_lane_canonical_frames
//     ├─ run_vision_depth_stage            (spatial/depth.*)
//     └─ run_vision_ocr_stage              (text/)
//          └─ run_vision_text_embedding_stage   (embeddings/)
//   run_vision_tracking_stage               (spatial/masks.*, spatial/regions.*,
//                                            entities/)
//   combine_vision_lane_results             (summary + processor records)
//
// No stage writes provenance/processors.jsonl: each returns its processor
// records and combine_vision_lane_results collects them for the caller.
//
// Each stage result round-trips through JSON (to_json / *_from_json) so it can
// cross a task boundary unchanged.

#include "svp/models/thread_plan.hpp"
#include "svp/package/spatial_embedding_placeholders.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/depth_frame_work.hpp"
#include "svp/vision/evidence_crop_work.hpp"
#include "svp/vision/inference_performance.hpp"
#include "svp/vision/keyframe_embedding_work.hpp"
#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/ocr_sample_plan.hpp"
#include "svp/vision/pp_ocr.hpp"
#include "svp/vision/text_embedding_work.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::media { struct MediaIngestPlan; }
namespace svp::vision { class FrameCatalog; }

namespace svp::package {

// Where a build sends the per-item work of its vision stages
// (svp/vision/dispatched_work.hpp). Every hook is empty unless the build is
// --distributed; an empty hook means the stage does the work itself.
struct VisionWorkDispatch {
  svp::vision::EvidenceCropDispatcher evidence_crops;
  svp::vision::TextEmbeddingDispatcher text_embeddings;
  svp::vision::KeyframeEmbeddingDispatcher keyframe_embeddings;
  svp::vision::DepthFrameDispatcher depth_frames;
};

// Inputs every vision stage shares. `media_plan` may be null (the legacy
// placeholder-only path used by package tests).
struct VisionLaneSettings {
  std::filesystem::path staging_dir;
  bool model_runtime_available = false;
  nlohmann::json media_plan_json;
  std::filesystem::path model_cache_root;
  const svp::media::MediaIngestPlan* media_plan = nullptr;
  std::filesystem::path ffmpeg_path;
  svp::vision::InferencePerformanceOptions performance;
  svp::models::ThreadPlan thread_plan;
  std::string visual_tracking_quality{
      svp::vision::kDefaultVisualTrackingQualityName};
  VisionWorkDispatch dispatch;
};

// Depth over the canonical frames, or the honest depth placeholder.
struct VisionDepthStageResult {
  bool depth_index_written = false;
  bool depth_blocks_written = false;
  bool depth_placeholder_written = false;
  bool depth_generation_run = false;
  bool depth_model_available = false;
  bool depth_model_verified = false;
  bool depth_frame_input_available = false;
  // Null when depth was not attempted (no model runtime).
  nlohmann::json depth_generation_detail;
  // The depth processor record, or the spatial placeholder record.
  nlohmann::json processor;
};

// OCR, numeric values, and evidence crops.
struct VisionOcrStageResult {
  bool ocr_available = false;
  bool ocr_frame_input_available = false;
  bool ocr_detection_run = false;
  bool ocr_recognition_run = false;
  std::size_t text_observation_count = 0;
  std::size_t numeric_value_count = 0;
  nlohmann::json ocr_generation_detail;
  std::vector<nlohmann::json> processors;
};

// Text embeddings over OCR observations, or the embedding placeholder.
struct VisionTextEmbeddingStageResult {
  bool embedding_sets_written = false;
  bool embeddings_index_written = false;
  bool embeddings_blocks_written = false;
  bool embedding_generation_run = false;
  bool embedding_model_available = false;
  // Null when embeddings were not attempted (no model runtime).
  nlohmann::json embedding_generation_detail;
  // The embedding processor record, or the embedding placeholder record.
  nlohmann::json processor;
};

// Visual entity tracking, masks, and regions, or the empty mask placeholder.
struct VisionTrackingStageResult {
  bool masks_index_written = false;
  bool masks_blocks_written = false;
  // The visual entity tracker's processor record; null when tracking did not
  // run.
  nlohmann::json processor;
};

// Decodes the canonical frames shared by depth and OCR. Empty (not attempted)
// without a media plan or ffmpeg path.
[[nodiscard]] svp::vision::DecodedCanonicalFrames
decode_vision_lane_canonical_frames(const VisionLaneSettings& settings,
                                    svp::vision::FrameCatalog* frame_catalog);

[[nodiscard]] VisionDepthStageResult run_vision_depth_stage(
    const VisionLaneSettings& settings,
    const svp::vision::DecodedCanonicalFrames& frames,
    const SpatialProgressCallback& on_progress);

[[nodiscard]] VisionOcrStageResult run_vision_ocr_stage(
    const VisionLaneSettings& settings,
    const svp::vision::DecodedCanonicalFrames& frames,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress);

// The OCR stage's options for these settings, exactly as run_vision_ocr_stage
// uses them. A build that runs OCR as frame-batch tasks plans the sample
// schedule (svp::vision::plan_ocr_samples) and PP-OCR options
// (svp::vision::make_ocr_pp_ocr_options) from these, so its batches decode
// and recognize exactly what the stage would. `on_progress` is captured by
// reference and must outlive the options' use.
[[nodiscard]] svp::vision::OcrGenerationOptions make_vision_ocr_options(
    const VisionLaneSettings& settings,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress);

// The OCR stage's reduce step for frame-batch results (plan §2.4 item 3): the
// result run_vision_ocr_stage would return when the stage ran the same sample
// plan in this process. `pp_ocr_session` (from `pp_ocr_options`) serves the
// evidence-crop re-read. Throws svp::vision::OcrFrameBatchReductionError when
// the batches do not cover the plan; other stage errors become the stage's
// blocker, as they do in run_vision_ocr_stage.
[[nodiscard]] VisionOcrStageResult run_vision_ocr_reduce_stage(
    const VisionLaneSettings& settings,
    const svp::vision::OcrSamplePlan& plan,
    std::vector<std::vector<svp::vision::OcrSampleDetections>> batch_results,
    const svp::vision::PpOcrSession& pp_ocr_session,
    const svp::vision::PpOcrOptions& pp_ocr_options,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress);

// Reads text/text_observations.jsonl written by the OCR stage.
[[nodiscard]] VisionTextEmbeddingStageResult run_vision_text_embedding_stage(
    const VisionLaneSettings& settings,
    const SpatialProgressCallback& on_progress);

// Reads timeline/shots.jsonl. Throws std::invalid_argument for an unknown
// tracking quality when tracking would run.
[[nodiscard]] VisionTrackingStageResult run_vision_tracking_stage(
    const VisionLaneSettings& settings,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress);

// The lane summary and the processor records the package's final stage
// merges, in the lane's order (OCR, then depth, then embeddings, then the
// tracker when tracking ran).
struct VisionLaneOutcome {
  SpatialEmbeddingPlaceholderSummary summary;
  std::vector<nlohmann::json> processor_records;
};

[[nodiscard]] VisionLaneOutcome combine_vision_lane_results(
    bool model_runtime_available,
    const VisionDepthStageResult& depth,
    const VisionOcrStageResult& ocr,
    const VisionTextEmbeddingStageResult& embeddings,
    const VisionTrackingStageResult& tracking);

[[nodiscard]] nlohmann::json to_json(const VisionDepthStageResult& result);
[[nodiscard]] nlohmann::json to_json(const VisionOcrStageResult& result);
[[nodiscard]] nlohmann::json to_json(const VisionTextEmbeddingStageResult& result);
[[nodiscard]] nlohmann::json to_json(const VisionTrackingStageResult& result);

// Inverses of to_json; throw nlohmann::json::exception for a malformed value.
[[nodiscard]] VisionDepthStageResult vision_depth_stage_result_from_json(
    const nlohmann::json& value);
[[nodiscard]] VisionOcrStageResult vision_ocr_stage_result_from_json(
    const nlohmann::json& value);
[[nodiscard]] VisionTextEmbeddingStageResult
vision_text_embedding_stage_result_from_json(const nlohmann::json& value);
[[nodiscard]] VisionTrackingStageResult vision_tracking_stage_result_from_json(
    const nlohmann::json& value);

}  // namespace svp::package
