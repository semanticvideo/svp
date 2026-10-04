#include "calibration/calibration_plan.hpp"

#include "calibration/audio_capacity_workloads.hpp"
#include "calibration/dispatched_capacity_workloads.hpp"

#include "svp/audio/tasks/diarize_window.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"

#include <algorithm>

namespace svp::builder::calibration {

std::vector<svp::vision::VisualTrackingQuality> calibrated_tracking_qualities() {
  if (!svp::vision::visual_tracking_enabled(svp::vision::kDefaultVisualTrackingQuality)) {
    return {};
  }
  return {svp::vision::kDefaultVisualTrackingQuality};
}

std::vector<CalibrationStep> calibration_steps(
    const DistributedVisionWork& vision, const DistributedAudioWork& audio,
    const std::vector<svp::vision::VisualTrackingQuality>& tracking_qualities) {
  std::vector<CalibrationStep> steps;
  steps.push_back({.kind = CalibrationKind::ocr,
                   .task_type = std::string(svp::vision::tasks::kOcrFrameBatchTaskType)});
  for (const std::string& type : dispatched_task_types(vision)) {
    steps.push_back({.kind = CalibrationKind::dispatched, .task_type = type});
  }
  for (const svp::vision::VisualTrackingQuality quality : tracking_qualities) {
    steps.push_back({.kind = CalibrationKind::tracking,
                     .task_type = std::string(svp::vision::tasks::kTrackWindowTaskType),
                     .tracking_quality = quality});
  }
  std::vector<std::string> audio_types = audio_task_types(audio);
  // diarize.window last (see the header).
  std::stable_partition(audio_types.begin(), audio_types.end(), [](const std::string& type) {
    return type != svp::audio::tasks::kDiarizeWindowTaskType;
  });
  for (const std::string& type : audio_types) {
    steps.push_back({.kind = CalibrationKind::audio, .task_type = type});
  }
  return steps;
}

}  // namespace svp::builder::calibration
