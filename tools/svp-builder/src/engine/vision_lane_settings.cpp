#include "engine/vision_lane_settings.hpp"

namespace svp::builder::engine {

svp::package::VisionLaneSettings make_vision_lane_settings(
    const BuildPipelineOptions& options, const svp::media::MediaIngestPlan& media_plan,
    const nlohmann::json& media_plan_json, const std::filesystem::path& staging_dir,
    const svp::models::ThreadPlan& thread_plan, bool model_runtime_available) {
  return svp::package::VisionLaneSettings{
      .staging_dir = staging_dir,
      .model_runtime_available = model_runtime_available,
      .media_plan_json = media_plan_json,
      .model_cache_root = options.model_cache_dir,
      .media_plan = &media_plan,
      .ffmpeg_path = options.ffmpeg_path,
      .performance = options.performance,
      .thread_plan = thread_plan,
      .visual_tracking_quality = options.visual_tracking_quality,
  };
}

}  // namespace svp::builder::engine
