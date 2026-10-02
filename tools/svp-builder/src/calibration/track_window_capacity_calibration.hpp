#pragma once

// Tracking capacity calibration (plan §3.5): how many track.window tasks a
// Mac should run at once, and how long one tracking frame takes there,
// measured on that Mac with the generic sweep (capacity_sweep.hpp) over the
// fixed calibration window (track_window_calibration_clip.hpp) and the
// build's tracking options (thread counts, detector, cadence).

#include "calibration/capacity_sweep.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/executor.hpp"
#include "svp/vision/tasks/track_window_spec.hpp"

#include <cstddef>
#include <cstdint>

namespace svp::builder::calibration {

// The warm-up window: two frames, the fewest a window tracks, so warming a
// slot loads its runtimes and runs cleanly (a worker returns only clean
// windows, track_window_task.hpp) at almost no cost.
inline constexpr std::size_t kTrackWindowCalibrationWarmUpFrames = 2;

// Workload name of the sweep's tasks ("task.calibration.track_window...").
inline constexpr std::string_view kTrackWindowCalibrationName = "track_window";

// The sweep's workload for `inputs` (its source is the calibration clip): the
// whole calibration window, timed, and a kTrackWindowCalibrationWarmUpFrames
// window that loads a slot's runtimes, untimed. Items are tracking frames.
[[nodiscard]] CapacityWorkload track_window_calibration_workload(
    const svp::vision::tasks::TrackWindowTaskInputs& inputs);

// The most slots worth trying: what free memory beyond the reserve admits at
// the calibration window's estimated peak, at most the logical CPUs, at
// least one.
[[nodiscard]] std::size_t track_window_calibration_max_slots(
    const svp::vision::tasks::TrackWindowTaskInputs& inputs, std::uint64_t available_bytes,
    std::uint64_t reserve_bytes, std::uint32_t logical_cpus);

[[nodiscard]] CapacityCalibration calibrate_track_window_capacity(
    svp::exec::Executor& executor, std::size_t max_slots,
    const svp::vision::tasks::TrackWindowTaskInputs& inputs,
    const svp::exec::CancellationToken& cancellation);

}  // namespace svp::builder::calibration
