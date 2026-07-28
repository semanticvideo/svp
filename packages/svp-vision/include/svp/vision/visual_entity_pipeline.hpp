#pragma once

#include "svp/models/reference_processor_model_ids.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/visual_entity_sampling.hpp"
#include "svp/vision/visual_entity_depth_schedule.hpp"
#include "svp/vision/visual_entity_cut_detection.hpp"
#include "svp/vision/visual_entity_detector.hpp"
#include "svp/vision/visual_entity_window_assembler.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace svp::vision {

using VisualEntityPipelineProgress =
    std::function<void(std::size_t current, std::size_t total)>;

struct VisualEntityPipelineOptions {
  VisualEntitySamplingOptions sampling;
  VisualEntityWindowAssemblerOptions assembly;
  VisualEntityDepthScheduleOptions depth_schedule;
  VisualEntityCutDetectionOptions cut_detection;
  VisualEntityDetectorOptions detector;
  std::string embedding_model_id = svp::models::kNomicEmbedVisionV15ModelId;
  std::string execution_provider = "cpu";
  VisualEntityPipelineProgress on_progress;
};

struct VisualEntityPipelineResult {
  AssembledVisualEntityResult assembled;
  std::size_t windows_planned = 0;
  std::size_t windows_processed = 0;
  std::size_t windows_succeeded = 0;
  std::size_t frames_attempted = 0;
  std::size_t frames_decoded = 0;
  std::size_t frames_missed = 0;
  std::vector<VisualEntityCutEvidence> cut_evidence;
  nlohmann::json failures = nlohmann::json::array();
  std::string blocker;
};

[[nodiscard]] VisualEntityPipelineResult run_visual_entity_pipeline(
    const media::MediaIngestPlan& media_plan,
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& model_cache_root,
    const std::vector<std::pair<std::string, std::int64_t>>& shot_boundaries,
    FrameCatalog* frame_catalog = nullptr,
    const VisualEntityPipelineOptions& options = {});

}  // namespace svp::vision
