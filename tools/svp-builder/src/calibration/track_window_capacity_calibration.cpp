#include "calibration/track_window_capacity_calibration.hpp"

#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/tasks/track_window_calibration_clip.hpp"
#include "svp/vision/visual_entity_window.hpp"

#include <stdexcept>

namespace svp::builder::calibration {
namespace {

// The calibration clip as a one-window plan, with the build's depth cadence.
svp::vision::VisualEntityPipelinePlan clip_plan(
    const svp::vision::tasks::TrackWindowTaskInputs& inputs, std::size_t frames) {
  const auto policy = svp::vision::visual_tracking_quality_policy(inputs.options.quality);
  svp::vision::VisualEntityPipelinePlan plan;
  plan.sampling = svp::vision::visual_entity_sampling_options(policy);
  plan.depth_schedule = inputs.options.depth_schedule;
  plan.depth_schedule.periodic_interval_us = policy.depth_interval_us;
  std::vector<std::int64_t> timestamps =
      svp::vision::tasks::track_window_calibration_timestamps_us(plan.sampling.sample_interval_us);
  timestamps.resize(std::min(frames, timestamps.size()));
  plan.duration_us = timestamps.back() + plan.sampling.sample_interval_us;
  plan.windows = {svp::vision::VisualEntitySamplingWindow{
      .start_us = 0, .end_us = timestamps.back(), .timestamps_us = std::move(timestamps)}};
  return plan;
}

svp::exec::TaskSpec clip_spec(const svp::vision::tasks::TrackWindowTaskInputs& inputs,
                              std::size_t frames) {
  if (!svp::vision::visual_tracking_enabled(inputs.options.quality)) {
    throw std::invalid_argument("tracking calibration needs tracking enabled");
  }
  const svp::vision::VisualEntityPipelinePlan plan = clip_plan(inputs, frames);
  svp::vision::FrameCatalog catalog;
  for (const std::int64_t timestamp : plan.windows.front().timestamps_us) {
    (void)catalog.register_frame(timestamp, svp::vision::kVisualEntityTrackingFramePurpose);
  }
  catalog.lock_to_plan();
  svp::vision::tasks::TrackWindowTaskInputs clip_inputs = inputs;
  clip_inputs.frame_width = svp::vision::tasks::kTrackWindowCalibrationFrameWidth;
  clip_inputs.frame_height = svp::vision::tasks::kTrackWindowCalibrationFrameHeight;
  return svp::vision::tasks::make_track_window_task_spec(clip_inputs, plan, 0, catalog);
}

}  // namespace

CapacityWorkload track_window_calibration_workload(
    const svp::vision::tasks::TrackWindowTaskInputs& inputs) {
  return CapacityWorkload{
      .name = std::string(kTrackWindowCalibrationName),
      .batch = clip_spec(inputs, svp::vision::tasks::kTrackWindowCalibrationFrames),
      .warm_up = clip_spec(inputs, kTrackWindowCalibrationWarmUpFrames),
      .items_per_batch = svp::vision::tasks::kTrackWindowCalibrationFrames,
  };
}

std::size_t track_window_calibration_max_slots(
    const svp::vision::tasks::TrackWindowTaskInputs& inputs, std::uint64_t available_bytes,
    std::uint64_t reserve_bytes, std::uint32_t logical_cpus) {
  const svp::exec::TaskSpec spec =
      clip_spec(inputs, svp::vision::tasks::kTrackWindowCalibrationFrames);
  return capacity_max_slots(available_bytes, reserve_bytes, logical_cpus,
                            spec.resources.est_peak_rss_mb);
}

CapacityCalibration calibrate_track_window_capacity(
    svp::exec::Executor& executor, std::size_t max_slots,
    const svp::vision::tasks::TrackWindowTaskInputs& inputs,
    const svp::exec::CancellationToken& cancellation) {
  return calibrate_capacity(executor, max_slots, track_window_calibration_workload(inputs),
                            cancellation);
}

}  // namespace svp::builder::calibration
