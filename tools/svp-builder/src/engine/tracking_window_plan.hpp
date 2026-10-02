#pragma once

// The visual tracking stage as window tasks (plan §2.4 item 9, §4.2, M4): the
// coordinator plans the stage's windows once, makes one track.window task per
// window that may run in this process or on paired workers, and the
// `tracking` stage task becomes the fold over their committed outcomes.
//
//   deps ─┬─ task.tracking.vstream_000.window_000000 ─┐
//         ├─ task.tracking.vstream_000.window_000001 ─┼─ task.tracking.vstream_000
//         └─ ...                                      ┘   (fold, artifacts)
//
// Windows depend on exactly what the whole tracking stage depended on, so
// lane order and concurrency are unchanged; the fold depends on every
// window. A window is the plan's own unit (its length and cadence belong to
// the tracking quality policy), so the task count is the window count and
// grows with the video's duration and the quality's window plan. Output is
// the same bytes whatever ran each window or in what order (each window is
// a function of its inputs, visual_entity_window.hpp; the fold assigns every
// ID in window order).
//
// Only a --distributed build splits the stage (a local build keeps running it
// whole, exactly as before); a resumed build splits it exactly when the
// journal it resumes recorded window tasks, so a journal resumes with or
// without --distributed, as before windows existed. And the stage splits only
// when every window can run the path the stage itself would run: a package build with the model
// runtime, a media plan, an ffmpeg whose build identity can be read, tracking
// enabled with at least one window, and verifiable bundles for the detector,
// depth, and embedding models that this Mac itself can load (if it cannot,
// a build without windows writes the stage's blockers, and workers that can
// must not fill the package with tracking instead). Otherwise the `tracking`
// task runs the whole stage in-process as before.

#include "engine/stage_task_plan.hpp"

#include "svp/builder/build_pipeline.hpp"
#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/tasks/track_window_spec.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_window.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder::engine {

// Everything a window task carries besides its window: the stage's options
// (explicit thread counts), the window plan, the model bundles, the decoder
// identity, the decode size, and the source.
struct TrackingWorkPlan {
  svp::vision::VisualEntityPipelineOptions options;
  svp::vision::VisualEntityPipelinePlan plan;
  std::vector<svp::exec::TaskModelRef> model_refs;
  std::string ffmpeg_build;
  int frame_width = 0;
  int frame_height = 0;
  svp::exec::ArtifactRef source;
  std::filesystem::path source_path;
};

struct TrackingWorkPlanInputs {
  const BuildPipelineOptions& options;
  const BuildStageExecutionPlan& stage_plan;
  const svp::media::MediaIngestPlan& media_plan;
  const nlohmann::json& media_plan_json;
  const svp::models::ThreadPlan& thread_plan;
  bool model_runtime_available = false;
  // The source's whole-file BLAKE3 and size (the journal's fingerprint).
  svp::exec::Blake3Digest source_blake3{};
  std::uint64_t source_bytes = 0;
};

// nullopt when the stage cannot split (see above); a --distributed build's
// planner calls it.
[[nodiscard]] std::optional<TrackingWorkPlan> plan_tracking_work(
    const TrackingWorkPlanInputs& inputs);

// Whether this build splits the tracking stage into windows: a resumed build
// as its journal did (`recorded_task_ids`), any other build when it is
// --distributed.
[[nodiscard]] bool split_tracking_stage(RecoveryJournalMode mode, bool distributed,
                                        const std::vector<std::string>& recorded_task_ids);

// True when this process can load the window runtimes (detector, depth, and
// embedding sessions) for `options` from `model_cache_root`. Loads them and
// lets them go.
[[nodiscard]] bool tracking_runtimes_load(const std::filesystem::path& model_cache_root,
                                          const svp::vision::VisualEntityPipelineOptions& options);

// The window tasks of one build, in window (canonical) order.
struct TrackingWindowPlan {
  TrackingWorkPlan work;
  std::vector<svp::exec::TaskNode> nodes;
};

// One task per window of `work.plan`, each depending on `depends_on` (the
// tracking stage task's own dependencies), with frame IDs from
// `planned_catalog` (the build's locked frame plan).
[[nodiscard]] TrackingWindowPlan make_tracking_window_plan(
    TrackingWorkPlan work, const svp::vision::tasks::TrackWindowCostPolicy& cost,
    const std::string& build_session_id, const std::vector<std::string>& depends_on,
    const svp::vision::FrameCatalog& planned_catalog);

// The tracking stage task's dependencies, as task IDs.
[[nodiscard]] std::vector<std::string> tracking_stage_dependencies(
    const std::vector<PlannedStageTask>& tasks);

}  // namespace svp::builder::engine
