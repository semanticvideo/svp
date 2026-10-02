#pragma once

// One visual tracking window as a unit of work (plan §2.4 item 9, §4.2
// `track.window`): the window plan, the model runtimes a window uses, and the
// per-window computation. A window's outcome is a function of the window,
// the source, the options, and the model bundles only: nothing carries over
// from an earlier window (GrabCut reseeds per call, visual_entity_grabcut.hpp;
// the runtimes keep no state between windows except counters this module
// reports per window). Identity across windows, every counter-based ID, and
// the stage's failure list are the fold's job
// (visual_entity_window_fold.hpp), which runs in window order.

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/depth_generation.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/visual_entity_cut_detection.hpp"
#include "svp/vision/visual_entity_depth_schedule.hpp"
#include "svp/vision/visual_entity_detector.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_sampling.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace svp::vision {

// The windows a tracking run decodes, from the media duration and the
// quality policy alone.
struct VisualEntityPipelinePlan {
  VisualEntitySamplingOptions sampling{};
  // options.depth_schedule with the quality policy's periodic interval.
  VisualEntityDepthScheduleOptions depth_schedule;
  std::int64_t duration_us = 0;
  std::vector<VisualEntitySamplingWindow> windows;
};

// Throws std::invalid_argument when the quality's depth cadence is not an
// integer multiple of its RGB cadence. Tracking must be enabled.
[[nodiscard]] VisualEntityPipelinePlan plan_visual_entity_pipeline(
    const media::MediaIngestPlan& media_plan,
    const VisualEntityPipelineOptions& options);

// The three model runtimes a window uses. Loaded once per process (or per
// pooled slot) and reused for every window it runs.
struct VisualEntityWindowRuntimes {
  VisualEntityDetectorRuntime detector;
  DepthInferenceRuntime depth;
  VisualEntityEmbeddingRuntime embedding;
};

// options.detector with options.execution_provider, as the stage loads it.
[[nodiscard]] VisualEntityDetectorOptions visual_entity_window_detector_options(
    const VisualEntityPipelineOptions& options);

[[nodiscard]] VisualEntityWindowRuntimes load_visual_entity_window_runtimes(
    const std::filesystem::path& model_cache_root,
    const VisualEntityPipelineOptions& options);

// True when the detector, depth, and embedding sessions all loaded.
[[nodiscard]] bool visual_entity_window_runtimes_complete(
    const VisualEntityWindowRuntimes& runtimes);

// What the fold needs to know about the runtimes every window ran with: the
// load blockers it records once, the detector refs and identity it adds to
// each window's provenance.
struct VisualEntityWindowRuntimeStatus {
  bool detector_loaded = false;
  std::string detector_blocker;
  std::vector<std::string> detector_model_refs;
  nlohmann::json detector_model_identity;
  bool depth_loaded = false;
  std::string depth_blocker;
};

[[nodiscard]] VisualEntityWindowRuntimeStatus visual_entity_window_runtime_status(
    const VisualEntityWindowRuntimes& runtimes);

// One failure a window recorded, in the order the window met it. The fold
// adds the window's time range and drops repeats exactly as the stage always
// has.
struct VisualEntityWindowFailure {
  std::string component;
  std::string message;
};

enum class VisualEntityWindowStatus {
  // Fewer than two frames decoded: the window contributes only its decode
  // counters and one frame_decode failure.
  decode_failed,
  // The tracker threw: failures (and cut evidence) only.
  tracker_failed,
  // tracker_result holds the window's raw tracker output.
  tracked,
  // The window did not run: this process could not load its runtimes
  // (failures holds why). Only the track.window task produces it, on the
  // coordinator; the stage then runs as it does without window tasks, so
  // its package and blockers are the ones such a build writes. The fold
  // never accepts it.
  not_started,
};

struct VisualEntityWindowOutcome {
  VisualEntityWindowStatus status = VisualEntityWindowStatus::decode_failed;
  std::size_t frames_attempted = 0;
  std::size_t frames_decoded = 0;
  std::size_t frames_missed = 0;
  // Timestamps of the frames that decoded, in decode order. The first one is
  // the window's keyframe registration (FrameCatalog::register_frame).
  std::vector<std::int64_t> decoded_timestamps_us;
  std::vector<VisualEntityWindowFailure> failures;
  // Every cut-detection record of the window, in frame order.
  std::vector<VisualEntityCutEvidence> cut_evidence;
  // Timestamps of the records above that are cuts.
  std::vector<std::int64_t> cut_timestamps_us;
  // The detector's counters for this window alone.
  VisualEntityDetectorDiagnostics detector_diagnostics;
  // The runtimes the window ran with; the fold of windows computed elsewhere
  // takes the stage's runtime status from them.
  VisualEntityWindowRuntimeStatus runtime_status;
  // Raw tracker output (status tracked): window-local IDs, before the fold
  // adds detector refs, limitations, and stage parameters.
  EntityTrackResult tracker_result;
};

// How a window's decoded frames get their package frame IDs.
struct VisualEntityWindowFrameIdentity {
  // In-process: the build's catalog, registered as frames decode (as the
  // stage always did). Null with no planned IDs gives fallback IDs.
  FrameCatalog* frame_catalog = nullptr;
  // Elsewhere: the coordinator's planned IDs and catalog indices, one per
  // window timestamp. Used when frame_catalog is null and these are set.
  std::vector<std::string> planned_frame_ids;
  std::vector<std::size_t> planned_frame_indices;
};

struct VisualEntityWindowRequest {
  std::filesystem::path source_path;
  std::filesystem::path ffmpeg_path;
  int frame_width = 0;
  int frame_height = 0;
  VisualEntitySamplingWindow window;
  VisualEntityDepthScheduleOptions depth_schedule;
  VisualEntityCutDetectionOptions cut_detection;
  std::string embedding_model_id;
  std::string execution_provider;
};

// Called between frames; throwing stops the window (cancellation).
using VisualEntityWindowCheckpoint = std::function<void()>;

// Decodes the window, runs the detector on every frame, depth on the
// scheduled frames, cut detection, and the tracker. Exceptions from
// detection, depth, and tracking are recorded as failures, as in the stage.
[[nodiscard]] VisualEntityWindowOutcome run_visual_entity_window(
    const VisualEntityWindowRequest& request,
    VisualEntityWindowRuntimes& runtimes,
    const VisualEntityWindowFrameIdentity& identity,
    const VisualEntityWindowCheckpoint& checkpoint = {});

// The request for `window` of a stage run with `options` over `media_plan`.
[[nodiscard]] VisualEntityWindowRequest make_visual_entity_window_request(
    const media::MediaIngestPlan& media_plan,
    const std::filesystem::path& ffmpeg_path,
    const VisualEntityPipelineOptions& options,
    const VisualEntityPipelinePlan& plan,
    std::size_t window_index);

}  // namespace svp::vision
