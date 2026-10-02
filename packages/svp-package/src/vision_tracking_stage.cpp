#include "svp/package/vision_lane_stages.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/package/entity_writer.hpp"
#include "svp/package/visual_entity_artifact_writer.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_window_fold.hpp"
#include "vision_lane_files.hpp"
#include "vision_lane_placeholders.hpp"

#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace svp::package {
namespace {

using RunTracking = std::function<svp::vision::VisualEntityPipelineResult(
    const svp::vision::VisualEntityPipelineOptions& options,
    const std::vector<std::pair<std::string, std::int64_t>>& shot_boundaries)>;

// The stage around one tracking run: shot boundaries, options with the
// artifact sink and progress, the run itself, the entity/region/mask
// artifacts, and the mask placeholder when nothing was written.
VisionTrackingStageResult tracking_stage(const VisionLaneSettings& settings,
                                         const SpatialProgressCallback& on_progress,
                                         const RunTracking& run) {
  VisionTrackingStageResult result;
  // Visual tracking owns its temporal coverage independently from the
  // five-frame foundation input shared by depth and embedding generation. It
  // processes bounded overlapping decode windows for dense temporal coverage
  // and cross-window identity handoff.
  if (vision_tracking_would_run(settings)) {
    std::vector<std::pair<std::string, std::int64_t>> shot_boundaries;
    for (const auto& shot : detail::read_jsonl_records(
             settings.staging_dir / "timeline" / "shots.jsonl")) {
      if (shot.contains("id") && shot.contains("start_us")) {
        shot_boundaries.emplace_back(shot["id"].get<std::string>(),
                                     shot["start_us"].get<std::int64_t>());
      }
    }

    svp::vision::VisualEntityPipelineOptions entity_options =
        make_vision_tracking_options(settings);
    if (svp::vision::visual_tracking_enabled(entity_options.quality)) {
      VisualEntityArtifactWriter artifact_writer(settings.staging_dir);
      entity_options.assembly.artifact_sink =
          [&artifact_writer](const std::vector<svp::vision::TrackedRegion>& regions,
                             const std::vector<svp::vision::MaskWriteEntry>& masks) {
            artifact_writer.append(regions, masks);
          };
      if (on_progress) {
        entity_options.on_progress = [&on_progress](std::size_t current,
                                                    std::size_t total) {
          on_progress("visual_tracking", current, total, "");
        };
      }

      auto entity_result = run(entity_options, shot_boundaries);

      std::set<std::string> retained_entity_ids;
      for (const auto& entity : entity_result.assembled.tracker_result.entities) {
        retained_entity_ids.insert(entity.entity_id);
      }
      const auto streamed_artifacts = artifact_writer.finish(retained_entity_ids);

      const auto visual_entity_summary = write_visual_entity_artifacts(
          settings.staging_dir, entity_result.assembled.tracker_result, nullptr,
          &streamed_artifacts);
      result.masks_index_written = visual_entity_summary.masks_written;
      result.masks_blocks_written = visual_entity_summary.masks_written;
      result.processor = visual_entity_summary.processor_record;
    }
  }

  if (!result.masks_index_written) {
    detail::write_mask_placeholder_files(settings.staging_dir);
    result.masks_index_written = true;
    result.masks_blocks_written = true;
  }
  return result;
}

}  // namespace

bool vision_tracking_would_run(const VisionLaneSettings& settings) {
  return settings.model_runtime_available && settings.media_plan != nullptr &&
         !settings.ffmpeg_path.empty();
}

svp::vision::VisualEntityPipelineOptions make_vision_tracking_options(
    const VisionLaneSettings& settings) {
  svp::vision::VisualEntityPipelineOptions entity_options;
  entity_options.execution_provider = "cpu";
  entity_options.detector.threads = settings.thread_plan.visual_entity_detection;
  entity_options.depth_threads = settings.thread_plan.depth;
  entity_options.embedding_threads = settings.thread_plan.visual_entity_embedding;
  const auto parsed_quality =
      svp::vision::parse_visual_tracking_quality(settings.visual_tracking_quality);
  if (!parsed_quality) {
    throw std::invalid_argument(
        "visual tracking quality must be off, low, medium, or high");
  }
  entity_options.quality = *parsed_quality;
  if (svp::vision::visual_tracking_enabled(entity_options.quality)) {
    entity_options.assembly.handoff_retention_us =
        svp::vision::visual_tracking_quality_policy(entity_options.quality)
            .window_overlap_us;
  }
  return entity_options;
}

VisionTrackingStageResult run_vision_tracking_stage(
    const VisionLaneSettings& settings,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress) {
  return tracking_stage(
      settings, on_progress,
      [&](const svp::vision::VisualEntityPipelineOptions& options,
          const std::vector<std::pair<std::string, std::int64_t>>& shot_boundaries) {
        return svp::vision::run_visual_entity_pipeline(
            *settings.media_plan, settings.ffmpeg_path, settings.model_cache_root,
            shot_boundaries, frame_catalog, options);
      });
}

VisionTrackingStageResult run_vision_tracking_reduce_stage(
    const VisionLaneSettings& settings,
    const svp::vision::VisualEntityPipelinePlan& plan,
    const VisualEntityWindowSource& source,
    const svp::vision::VisualEntityWindowRuntimeStatus& runtimes,
    svp::vision::FrameCatalog* frame_catalog,
    const SpatialProgressCallback& on_progress) {
  if (!vision_tracking_would_run(settings) || plan.windows.empty() || !source) {
    throw std::logic_error(
        "the tracking reduce step needs a tracking run's window plan and outcomes");
  }
  return tracking_stage(
      settings, on_progress,
      [&](const svp::vision::VisualEntityPipelineOptions& options,
          const std::vector<std::pair<std::string, std::int64_t>>&) {
        if (!svp::vision::visual_tracking_enabled(options.quality)) {
          throw std::logic_error("the tracking reduce step needs tracking enabled");
        }
        svp::vision::VisualEntityWindowFold fold(options, plan, runtimes, frame_catalog);
        for (std::size_t index = 0; index < plan.windows.size(); ++index) {
          fold.append(index, source(index));
        }
        return fold.finish();
      });
}

}  // namespace svp::package
