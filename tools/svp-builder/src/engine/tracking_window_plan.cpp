#include "engine/tracking_window_plan.hpp"

#include "engine/vision_lane_settings.hpp"

#include "svp/package/vision_lane_stages.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::builder::engine {
namespace {

// The source is passed to ffmpeg as-is; its container is ffmpeg's business,
// so the reference names no particular one.
constexpr const char* kSourceMediaType = "application/octet-stream";

}  // namespace

std::optional<TrackingWorkPlan> plan_tracking_work(const TrackingWorkPlanInputs& inputs) {
  if (!inputs.stage_plan.run_package_skeleton) {
    return std::nullopt;
  }
  const svp::package::VisionLaneSettings settings = make_vision_lane_settings(
      inputs.options, inputs.media_plan, inputs.media_plan_json, {}, inputs.thread_plan,
      inputs.model_runtime_available);
  if (!svp::package::vision_tracking_would_run(settings)) {
    return std::nullopt;
  }
  TrackingWorkPlan work;
  try {
    // An unknown quality, or a quality whose cadences do not fit together,
    // is the whole stage's error to report.
    work.options = svp::package::make_vision_tracking_options(settings);
    if (!svp::vision::visual_tracking_enabled(work.options.quality)) {
      return std::nullopt;
    }
    work.plan = svp::vision::plan_visual_entity_pipeline(inputs.media_plan, work.options);
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
  if (work.plan.windows.empty()) {
    return std::nullopt;
  }
  if (!svp::vision::ffmpeg_executable_available(settings.ffmpeg_path)) {
    return std::nullopt;
  }
  std::optional<std::string> ffmpeg_build =
      svp::vision::tasks::cached_ffmpeg_build_identity(settings.ffmpeg_path);
  if (!ffmpeg_build) {
    return std::nullopt;
  }
  try {
    work.model_refs =
        svp::vision::tasks::track_window_model_refs(settings.model_cache_root, work.options);
  } catch (const std::exception&) {
    // No verifiable bundles: the whole stage reports its own limitations.
    return std::nullopt;
  }
  if (!tracking_runtimes_load(settings.model_cache_root, work.options)) {
    // This Mac would track with blockers; so must the build.
    return std::nullopt;
  }
  work.ffmpeg_build = std::move(*ffmpeg_build);
  work.frame_width = inputs.media_plan.canonical_raster.width;
  work.frame_height = inputs.media_plan.canonical_raster.height;
  work.source = svp::exec::ArtifactRef{
      .blake3 = inputs.source_blake3,
      .bytes = inputs.source_bytes,
      .media_type = kSourceMediaType,
      .role = std::string(svp::vision::tasks::kTrackWindowSourceRole)};
  work.source_path = inputs.options.source_path;
  return work;
}

bool split_tracking_stage(RecoveryJournalMode mode, bool distributed,
                          const std::vector<std::string>& recorded_task_ids) {
  if (mode != RecoveryJournalMode::resume) {
    return distributed;
  }
  return std::any_of(recorded_task_ids.begin(), recorded_task_ids.end(),
                     [](const std::string& id) {
                       return id.starts_with(svp::vision::tasks::kTrackWindowTaskIdPrefix);
                     });
}

bool tracking_runtimes_load(const std::filesystem::path& model_cache_root,
                            const svp::vision::VisualEntityPipelineOptions& options) {
  return svp::vision::visual_entity_window_runtimes_complete(
      svp::vision::load_visual_entity_window_runtimes(model_cache_root, options));
}

TrackingWindowPlan make_tracking_window_plan(TrackingWorkPlan work,
                                             const svp::vision::tasks::TrackWindowCostPolicy& cost,
                                             const std::string& build_session_id,
                                             const std::vector<std::string>& depends_on,
                                             const svp::vision::FrameCatalog& planned_catalog) {
  TrackingWindowPlan plan;
  plan.work = std::move(work);
  const svp::vision::tasks::TrackWindowTaskInputs inputs{
      .build_session_id = build_session_id,
      .depends_on = depends_on,
      .source = plan.work.source,
      .model_refs = plan.work.model_refs,
      .options = plan.work.options,
      .frame_width = plan.work.frame_width,
      .frame_height = plan.work.frame_height,
      .ffmpeg_build = plan.work.ffmpeg_build,
      .cost = cost,
  };
  plan.nodes.reserve(plan.work.plan.windows.size());
  for (std::size_t index = 0; index < plan.work.plan.windows.size(); ++index) {
    plan.nodes.push_back(svp::exec::TaskNode{
        .spec = svp::vision::tasks::make_track_window_task_spec(inputs, plan.work.plan, index,
                                                                planned_catalog),
        .order_key = svp::vision::tasks::track_window_order_key(index)});
  }
  return plan;
}

std::vector<std::string> tracking_stage_dependencies(const std::vector<PlannedStageTask>& tasks) {
  for (const PlannedStageTask& task : tasks) {
    if (task.kind != StageTaskKind::tracking) {
      continue;
    }
    std::vector<std::string> ids;
    for (const StageTaskKind dependency : task.depends_on) {
      ids.emplace_back(stage_task_id(dependency));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
  }
  throw std::logic_error("the build plan has no tracking stage task");
}

}  // namespace svp::builder::engine
