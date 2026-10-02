#pragma once

// OCR capacity calibration (plan §3.5): how many ocr.frame_batch tasks a Mac
// should run at once, measured on that Mac rather than assumed from its chip,
// core count, or memory size.
//
// The sweep runs the fixed calibration slice (ocr_calibration_clip.hpp) on
// one executor at 1, 2, 3, ... concurrent slots. Each step runs `slots`
// independent chains of kOcrCalibrationBatchesPerSlot whole-slice batches
// (so exactly `slots` tasks are in flight), after a barrier on the previous
// step, and is timed from its first lease to its last commit. Before each
// step, `slots` concurrent one-frame warm-up batches run untimed, so every
// slot already holds a loaded PP-OCR session and file caches are warm: model
// loading never counts against a step.
// The sweep stops at the first step whose throughput (frames per second)
// gains less than kCapacityMinSlotGain over the best step so far, or at
// `max_slots` (the most slots the Mac's memory admits, never more than its
// logical CPUs). The knee, the best step before that, is the slot count;
// seconds per frame at the knee (wall x slots / frames) sizes batches.

#include "calibration/capacity_sweep.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/executor.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace svp::builder::calibration {

// The sweep is calibrate_capacity's (capacity_sweep.hpp, which owns the
// slot-gain threshold); this header keeps OCR's record shape (frames).
inline constexpr std::size_t kOcrCalibrationBatchesPerSlot = kCapacityBatchesPerSlot;

struct OcrCalibrationStep {
  std::size_t slots = 0;
  std::uint64_t frames = 0;
  std::int64_t wall_ms = 0;
  double frames_per_second = 0.0;
};

struct OcrCalibration {
  std::size_t slots = 0;
  double seconds_per_frame = 0.0;
  std::vector<OcrCalibrationStep> sweep;
  std::string stopped_because;
};

// The sweep's task graph: for each step k in 1..max_slots, k one-frame
// warm-up tasks after the previous step, then k timed chains of
// kOcrCalibrationBatchesPerSlot batches after all k warm-ups. `timed_step`
// maps each timed task to its step; warm-ups are not in it.
struct OcrCalibrationGraph {
  std::vector<svp::exec::TaskNode> nodes;
  std::map<std::string, std::size_t> timed_step;
};
[[nodiscard]] OcrCalibrationGraph ocr_calibration_graph(
    const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
    const std::vector<std::int64_t>& timestamps_us, std::size_t max_slots);

[[nodiscard]] nlohmann::json ocr_calibration_to_json(const OcrCalibration& calibration);
// Throws nlohmann::json::exception or std::invalid_argument for a malformed
// value.
[[nodiscard]] OcrCalibration ocr_calibration_from_json(const nlohmann::json& value);

// The most slots worth trying on a Mac: what its free memory (beyond its
// admission reserve) admits at kOcrFrameBatchEstimatedPeakRssMb per slot,
// at most its logical CPUs (a slot always needs a core), at least one.
[[nodiscard]] std::size_t ocr_calibration_max_slots(std::uint64_t available_bytes,
                                                    std::uint64_t reserve_bytes,
                                                    std::uint32_t logical_cpus);

// The TaskSpec of one calibration batch (all clip frames), for its
// parameters digest: a calibration is valid only for these parameters.
[[nodiscard]] svp::exec::TaskSpec ocr_calibration_spec(
    const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
    const std::vector<std::int64_t>& timestamps_us);

// Runs the sweep on `executor`, which must accept ocr.frame_batch and
// advertise at least `max_slots` slots. `inputs.source` is the calibration
// clip, already resolvable by the executor. Throws std::runtime_error when an
// attempt fails or is lost (a retried step would time the retry, not the
// Mac), so an interrupted sweep is never kept.
[[nodiscard]] OcrCalibration calibrate_ocr_capacity(
    svp::exec::Executor& executor, std::size_t max_slots,
    const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
    const std::vector<std::int64_t>& timestamps_us,
    const svp::exec::CancellationToken& cancellation);

}  // namespace svp::builder::calibration
