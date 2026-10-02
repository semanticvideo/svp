#pragma once

#include "svp/media/canonical_raster.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/visual_entity_cut_detection.hpp"
#include "svp/vision/visual_entity_depth_schedule.hpp"
#include "svp/vision/visual_entity_detector.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_sampling.hpp"
#include "svp/vision/visual_entity_window.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Task type identity (plan §4.2 `track.window`). The version changes whenever
// the parameter schema, the payload format, or the meaning of either changes.
inline constexpr std::string_view kTrackWindowTaskType = "track.window";
inline constexpr std::uint64_t kTrackWindowTaskTypeVersion = 1;
// TaskSpec input holding the source media bytes.
inline constexpr std::string_view kTrackWindowSourceInput = "source";
inline constexpr std::string_view kTrackWindowSourceRole = "source_media";
// The one output: the window outcome (visual_entity_window_codec.hpp).
inline constexpr std::string_view kTrackWindowOutcomeRole = "visual_entity_window_outcome";
inline constexpr std::string_view kTrackWindowOutcomeMediaType = "application/cbor";

// Everything a track.window task needs besides its source bytes and the
// worker's own model cache and ffmpeg: the window (its timestamps and the
// coordinator's planned frame IDs for them), the decode size, and every value
// of the stage's options that can change a window's outcome, thread counts
// included (plan §2.4 item 5). Workers never fill a value from their own
// host. Values only the fold uses (window assembly) are not sent.
struct TrackWindowParameters {
  std::uint64_t window_ordinal = 0;
  VisualEntitySamplingWindow window;
  // One per window timestamp: the build's planned frame ID and catalog index.
  std::vector<std::string> frame_ids;
  std::vector<std::uint64_t> frame_indices;
  int frame_width = 0;
  int frame_height = 0;
  // ffmpeg_build_identity() of the coordinator's ffmpeg ("b3:<hex>").
  std::string ffmpeg_build;
  VisualEntityDepthScheduleOptions depth_schedule;
  VisualEntityCutDetectionOptions cut_detection;
  // execution_provider is the top-level one.
  VisualEntityDetectorOptions detector;
  svp::models::OrtThreadCounts depth_threads;
  svp::models::OrtThreadCounts embedding_threads;
  std::string embedding_model_id;
  std::string execution_provider = "cpu";
};

// The model a window's depth runtime loads (load_visual_entity_window_runtimes).
[[nodiscard]] std::string track_window_depth_model_id();

// The stage options a window runs with: the parameters' detector, model IDs,
// threads, and execution provider. Fold-only options keep their defaults.
[[nodiscard]] VisualEntityPipelineOptions track_window_pipeline_options(
    const TrackWindowParameters& parameters);

// The window request for `source_path` decoded by `ffmpeg_path`.
[[nodiscard]] VisualEntityWindowRequest track_window_request(
    const TrackWindowParameters& parameters, const std::filesystem::path& source_path,
    const std::filesystem::path& ffmpeg_path);

// Canonical parameters object:
//   {"cut_detection":{...},
//    "decode":{"ffmpeg_build","frame_height","frame_width"},
//    "depth":{"model_id","threads":{"inter_op","intra_op"}},
//    "depth_schedule":{"periodic_interval_us","scene_change_burst_frames",
//                      "scene_change_threshold"},
//    "detector":{<every VisualEntityDetectorOptions threshold>,"model_id",
//                "threads":{"inter_op","intra_op"}},
//    "embedding":{"model_id","threads":{"inter_op","intra_op"}},
//    "execution_provider",
//    "window":{"end_us","frame_ids":[...],"frame_indices":[...],"ordinal",
//              "start_us","timestamps_us":[...]}}
// Throws std::invalid_argument when the values break the schema (for example
// a thread count left at kRuntimeChoosesThreadCount, which would let each
// worker pick its own).
[[nodiscard]] nlohmann::json track_window_parameters_to_json(
    const TrackWindowParameters& parameters);

// Strict inverse; throws std::invalid_argument with the validator's reason.
[[nodiscard]] TrackWindowParameters track_window_parameters_from_json(
    const nlohmann::json& value);

// The registry's parameter validator: nullopt when `value` is valid. Unknown,
// missing, or mistyped fields; fewer than two window timestamps, timestamps
// that are not a uniform ascending cadence inside [start_us, end_us], frame
// IDs or indices that do not pair one-to-one with them; a decode size outside
// 1..svp::media::kCanonicalLongestDisplayDimension (windows
// decode the canonical analysis raster); a thread count below 1; a depth model
// other than the one the window loads; and thresholds outside their ranges
// are all rejected.
[[nodiscard]] std::optional<std::string> validate_track_window_parameters(
    const nlohmann::json& value);

}  // namespace svp::vision::tasks
