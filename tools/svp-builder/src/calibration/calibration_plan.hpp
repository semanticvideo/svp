#pragma once

// Which capacity measurements a Mac needs before a --distributed build can
// dispatch work to it (plan §3.5), and in which order this Mac takes them.
//
// `workers pair`, `workers sync`, and `workers fleet pair` take every one of
// them ahead of time, for this Mac and the worker, so a build on the same
// runtime and conditions finds every record current and measures nothing.
// The kinds are:
//   * ocr: ocr.frame_batch (ocr_capacity_calibration.hpp);
//   * dispatched: each dispatched vision type (dispatched_task_types);
//   * tracking: track.window at the default tracking quality (each quality
//     keeps its own record, track_window_calibration_runs.hpp; a build at
//     another quality measures its own once);
//   * audio: each dispatched audio type (audio_task_types).
// diarize.window comes last: on this Mac it runs in-process and loads
// sherpa-onnx, which must not load before ONNX Runtime models
// (svp/audio/sherpa_diarization.hpp), and every other in-process
// measurement loads such models.

#include "svp/builder/distributed_execution.hpp"
#include "svp/vision/visual_tracking_quality.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace svp::builder::calibration {

enum class CalibrationKind { ocr, dispatched, tracking, audio };

struct CalibrationStep {
  CalibrationKind kind = CalibrationKind::ocr;
  // The task type the step measures (track.window for every tracking step).
  std::string task_type;
  // Tracking steps: which quality.
  svp::vision::VisualTrackingQuality tracking_quality = svp::vision::VisualTrackingQuality::off;
};

// The tracking qualities measured ahead of builds: the default build's.
[[nodiscard]] std::vector<svp::vision::VisualTrackingQuality> calibrated_tracking_qualities();

// The measurements for a build dispatching `vision` and `audio`, with a
// tracking step per entry of `tracking_qualities`, in the order above.
[[nodiscard]] std::vector<CalibrationStep> calibration_steps(
    const DistributedVisionWork& vision, const DistributedAudioWork& audio,
    const std::vector<svp::vision::VisualTrackingQuality>& tracking_qualities);

}  // namespace svp::builder::calibration
