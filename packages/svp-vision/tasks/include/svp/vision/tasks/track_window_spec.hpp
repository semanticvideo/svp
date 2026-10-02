#pragma once

// Coordinator side of track.window: the TaskSpec for one window of a visual
// tracking plan, and reading its output back for the fold
// (svp::vision::VisualEntityWindowFold).

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_window.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Scheduling lane of track.window tasks; ordered by window ordinal.
inline constexpr std::string_view kTrackWindowLane = "track.window";

// Admission estimate of one window's peak resident memory (plan §4.4):
//   kTrackWindowRuntimeResidentMb
//     + frames x raster pixels x (kTrackWindowFrameBytesPerPixel
//                                 + detector maximum_detections).
// kTrackWindowRuntimeResidentMb: measured peak resident memory of a
// one-window tracking run is a little over 1.1 GB, nearly all of it the
// loaded detector, depth, and embedding sessions (which do not grow with the
// window); 1,200 MB rounds that up so admission never under-reserves. Per
// frame and pixel a window holds the decoded RGB frame (3 bytes) and at most
// one depth value (2 bytes), plus one mask byte per region the tracker keeps
// for the frame, bounded by the detector's maximum_detections (motion and
// depth candidates that overlap a detector box fuse with it rather than
// adding a region). The estimate sizes admission and slots only; it never
// affects output.
inline constexpr std::uint64_t kTrackWindowRuntimeResidentMb = 1200;
inline constexpr std::uint64_t kTrackWindowFrameBytesPerPixel = 3 + 2;

// How long a window is expected to take, for leases and the attempt
// deadline only (lease_policy.hpp); never for output. The default is the
// order of the per-frame cost measured for tracking at medium on real
// footage with detections (plan §2.2), so leases and deadlines are generous
// for a Mac that has not measured itself; a --distributed build replaces it
// with this Mac's measured cost.
inline constexpr double kTrackWindowDefaultSecondsPerFrame = 0.81;

// The estimate above for a window of `frames` frames of a width x height
// raster with the detector's `maximum_detections`. A coordinator sizes its
// own window slots with it for the build's largest window.
[[nodiscard]] std::uint64_t track_window_estimated_peak_rss_mb(std::size_t frames, int width,
                                                               int height,
                                                               std::size_t maximum_detections);

struct TrackWindowCostPolicy {
  double estimated_seconds_per_frame = kTrackWindowDefaultSecondsPerFrame;
};

struct TrackWindowTaskInputs {
  std::string build_session_id;
  std::vector<std::string> depends_on;
  // The source media, role kTrackWindowSourceRole.
  svp::exec::ArtifactRef source;
  // From track_window_model_refs().
  std::vector<svp::exec::TaskModelRef> model_refs;
  // The stage's options: detector, model IDs, explicit thread counts,
  // execution provider, and cut detection.
  VisualEntityPipelineOptions options;
  int frame_width = 0;
  int frame_height = 0;
  // ffmpeg_build_identity() of the coordinator's ffmpeg.
  std::string ffmpeg_build;
  TrackWindowCostPolicy cost;
};

// TaskModelRefs for the detector, depth, and embedding bundles `options`
// loads, read from their manifests under `model_cache_root`. Throws
// std::runtime_error when a manifest is missing or malformed.
[[nodiscard]] std::vector<svp::exec::TaskModelRef> track_window_model_refs(
    const std::filesystem::path& model_cache_root, const VisualEntityPipelineOptions& options);

// Every window task ID starts with this.
inline constexpr std::string_view kTrackWindowTaskIdPrefix = "task.tracking.vstream_000.window_";

// kTrackWindowTaskIdPrefix and a six-digit (minimum) window ordinal.
[[nodiscard]] std::string track_window_task_id(std::size_t window_index);

[[nodiscard]] svp::exec::TaskOrderKey track_window_order_key(std::size_t window_index);

// The validated TaskSpec for window `window_index` of `plan`, whose frames
// carry the IDs `planned_catalog` (the build's locked frame plan) assigns.
// cache_key is compute_cache_key(["track.window", version, source blake3,
// [model_bundle_id...], parameters_blake3]). Throws std::invalid_argument
// for a window outside the plan, a timestamp missing from the catalog, or
// parameters that fail validation.
[[nodiscard]] svp::exec::TaskSpec make_track_window_task_spec(
    const TrackWindowTaskInputs& inputs, const VisualEntityPipelinePlan& plan,
    std::size_t window_index, const FrameCatalog& planned_catalog);

// Decodes a committed track.window output and checks it belongs to the
// spec's window: its decoded timestamps are a prefix of the window's.
// Throws VisualEntityWindowCodecError for bad bytes and std::invalid_argument
// for an outcome that does not match the spec.
[[nodiscard]] VisualEntityWindowOutcome read_track_window_output(
    const svp::exec::TaskSpec& spec, std::span<const std::uint8_t> payload);

}  // namespace svp::vision::tasks
