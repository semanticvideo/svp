#pragma once

// Capacity calibration of one task type on one executor (plan §3.5): how many
// tasks of that type a Mac should run at once, measured on that Mac rather
// than assumed from its chip, core count, or memory size.
//
// The sweep runs a fixed workload (one batch spec) at 1, 2, 3, ...
// concurrent slots. Each step runs `slots` independent copies of the batch
// (so exactly `slots` tasks are in flight), after a barrier on the previous
// step, and is timed from its first lease to its last commit. Before each
// step, `slots` concurrent one-item warm-up tasks run untimed, so every slot
// already holds a loaded model and file caches are warm: model loading never
// counts against a step. The sweep stops at the first step whose throughput
// (items per second) gains less than kCapacityMinSlotGain over the best step
// so far, or at `max_slots`. The knee, the best step before that, is the slot
// count; seconds per item at the knee (wall x slots / items) sizes batches.
//
// ocr_capacity_calibration.hpp measures ocr.frame_batch this way; the
// dispatched vision types (dispatched_capacity_workloads.hpp) too.

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/executor.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace svp::builder::calibration {

// Another slot is worth its memory and contention only if it adds at least
// 10% throughput: below that the gain is within run-to-run noise (plan §2.2 C
// measured +16% for a second `background` OCR slot and -4% for a second
// `fast` slot on an M4 mini), and an extra slot makes every task take
// longer, which lengthens a stage's tail.
inline constexpr double kCapacityMinSlotGain = 0.10;
// Timed copies of the batch each slot runs per step. One per slot already
// keeps every slot busy for the whole step (the copies are identical, so
// they start and end together); more would only lengthen calibration.
inline constexpr std::size_t kCapacityBatchesPerSlot = 1;

struct CapacityStep {
  std::size_t slots = 0;
  std::uint64_t items = 0;
  std::int64_t wall_ms = 0;
  double items_per_second = 0.0;
};

struct CapacityCalibration {
  std::size_t slots = 0;
  double seconds_per_item = 0.0;
  std::vector<CapacityStep> sweep;
  std::string stopped_because;
};

// What one sweep measures: `batch` (with `items_per_batch` items) is the
// timed unit, `warm_up` a one-item task of the same type and settings.
// `name` names the calibration tasks ("task.calibration.<name>.slots_...").
struct CapacityWorkload {
  std::string name;
  svp::exec::TaskSpec batch;
  svp::exec::TaskSpec warm_up;
  std::uint64_t items_per_batch = 0;
};

// The sweep's task graph: for each step k in 1..max_slots, k warm-up tasks
// after the previous step, then k timed copies of the batch after all k
// warm-ups. `timed_step` maps each timed task to its step.
struct CapacityCalibrationGraph {
  std::vector<svp::exec::TaskNode> nodes;
  std::map<std::string, std::size_t> timed_step;
};
[[nodiscard]] CapacityCalibrationGraph capacity_calibration_graph(
    const CapacityWorkload& workload, std::size_t max_slots);

// The most slots worth trying on a Mac: what its free memory (beyond its
// admission reserve) admits at `peak_rss_mb` per slot, at most its logical
// CPUs (a slot always needs a core), at least one.
[[nodiscard]] std::size_t capacity_max_slots(std::uint64_t available_bytes,
                                             std::uint64_t reserve_bytes,
                                             std::uint32_t logical_cpus,
                                             std::uint64_t peak_rss_mb);

// Runs the sweep on `executor`, which must accept the workload's type and
// advertise at least `max_slots` slots, with the workload's inputs already
// resolvable by it. Throws std::runtime_error when an attempt fails or is
// lost (a retried step would time the retry, not the Mac), so an
// interrupted sweep is never kept.
[[nodiscard]] CapacityCalibration calibrate_capacity(
    svp::exec::Executor& executor, std::size_t max_slots, const CapacityWorkload& workload,
    const svp::exec::CancellationToken& cancellation);

[[nodiscard]] nlohmann::json capacity_calibration_to_json(const CapacityCalibration& calibration);
// Throws nlohmann::json::exception or std::invalid_argument for a malformed
// value.
[[nodiscard]] CapacityCalibration capacity_calibration_from_json(const nlohmann::json& value);

}  // namespace svp::builder::calibration
