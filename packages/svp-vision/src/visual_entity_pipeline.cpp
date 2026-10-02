#include "svp/vision/visual_entity_pipeline.hpp"

#include "svp/vision/visual_entity_window.hpp"
#include "svp/vision/visual_entity_window_fold.hpp"

namespace svp::vision {

VisualEntityPipelineResult run_visual_entity_pipeline(
    const media::MediaIngestPlan& media_plan,
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& model_cache_root,
    const std::vector<std::pair<std::string, std::int64_t>>& shot_boundaries,
    FrameCatalog* frame_catalog,
    const VisualEntityPipelineOptions& options) {
  VisualEntityPipelineResult result;
  if (!visual_tracking_enabled(options.quality)) {
    return result;
  }

  // The package's current shot timeline is one range per foundation frame,
  // not a cinematic-cut contract. Entity tracking derives cut boundaries
  // from its own dense window frames instead.
  (void)shot_boundaries;
  const VisualEntityPipelinePlan plan = plan_visual_entity_pipeline(media_plan, options);
  if (plan.windows.empty()) {
    result.blocker = "video duration is unavailable for visual entity tracking";
    return result;
  }

  // Windows run one after another on this thread and are folded in window
  // order; each window is independent of the ones before it
  // (visual_entity_window.hpp), so this is the same reduction a build runs
  // over windows computed elsewhere.
  VisualEntityWindowRuntimes runtimes =
      load_visual_entity_window_runtimes(model_cache_root, options);
  // Frames register in `frame_catalog` as they decode, so the fold has no
  // catalog of its own to update.
  VisualEntityWindowFold fold(options, plan, visual_entity_window_runtime_status(runtimes),
                              nullptr);
  const VisualEntityWindowFrameIdentity identity{.frame_catalog = frame_catalog};
  for (std::size_t window_index = 0; window_index < plan.windows.size(); ++window_index) {
    fold.append(window_index,
                run_visual_entity_window(
                    make_visual_entity_window_request(media_plan, ffmpeg_path, options, plan,
                                                      window_index),
                    runtimes, identity));
  }
  return fold.finish();
}

}  // namespace svp::vision
