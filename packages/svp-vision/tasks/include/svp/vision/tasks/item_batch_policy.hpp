#pragma once

// How the per-item work of a dispatched vision stage (evidence crops, text
// and keyframe embeddings, depth frames; svp/vision/dispatched_work.hpp) is
// cut into tasks (plan §4.4 "dynamic balancing").
//
// Batch size only changes how work is scheduled, never the output: every
// task returns one record per item and the stage consumes the records in
// item order, so any partition reduces to the same bytes.
//
// Sizing is by time and by bytes, never by a fixed item count, so it scales
// with however many items a video has and however dense the owner makes the
// sampling (plan "no sampling assumptions"):
//   * a batch holds as many items as fit in target_task_seconds at the
//     measured per-item cost (the coordinator's calibration), so a lost lease
//     stays cheap to retry and pull scheduling can balance uneven Macs, while
//     per-task overhead stays small relative to the work;
//   * a batch never needs more task parameters than max_parameter_bytes nor
//     more result bytes than max_result_bytes (each item's share estimated
//     by the caller), so no TaskSpec or RESULT outgrows the protocol's frame
//     limits however large or numerous the items.

#include "svp/exec/frame_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace svp::vision::tasks {

// Wall time one dispatched task should take: the OCR frame-batch target
// (svp::vision::OcrBatchPolicy, plan §4.4), for the same reasons: long
// enough that leasing, framing, and commit overhead (milliseconds) is
// negligible, short enough that a lost worker costs little to redo and the
// last tasks of a stage finish close together.
inline constexpr double kItemTaskTargetSeconds = 10.0;

// A task's parameters travel inside its ASSIGN frame header, whose size the
// protocol bounds (svp::exec::kDefaultMaxFrameHeaderBytes). A quarter of it
// leaves the rest for the TaskSpec envelope (IDs, model refs, inputs, cache
// key) and the lease, with margin for estimates that run short.
inline constexpr std::uint64_t kMaxBatchParameterBytes =
    svp::exec::kDefaultMaxFrameHeaderBytes / 4;
// A task's data output travels as one RESULT payload, which the protocol
// bounds (svp::exec::kDefaultMaxFramePayloadBytes). Half of it absorbs
// estimates that run short (an image encoding larger than expected) without
// a result ever being refused.
inline constexpr std::uint64_t kMaxBatchResultBytes = svp::exec::kDefaultMaxFramePayloadBytes / 2;

struct ItemBatchPolicy {
  double target_task_seconds = kItemTaskTargetSeconds;
  // Measured cost of one item on one slot (> 0).
  double estimated_seconds_per_item = 0.0;
  // Largest parameters, and largest results, one batch may carry (> 0).
  std::uint64_t max_parameter_bytes = kMaxBatchParameterBytes;
  std::uint64_t max_result_bytes = kMaxBatchResultBytes;
};

// One item's share of a batch's parameters and results.
struct ItemBytes {
  std::uint64_t parameter_bytes = 0;
  std::uint64_t result_bytes = 0;
};

// A contiguous run of items [first, first + count).
struct ItemBatch {
  std::uint64_t first = 0;
  std::uint64_t count = 0;

  bool operator==(const ItemBatch&) const = default;
};

// Throws std::invalid_argument unless the times are finite and > 0 and both
// byte budgets are > 0.
void validate_item_batch_policy(const ItemBatchPolicy& policy);

// Items per batch by time alone: floor(target / estimate), at least one.
[[nodiscard]] std::uint64_t item_batch_size_by_time(const ItemBatchPolicy& policy);

// Cuts [0, item_count) into consecutive batches of at most
// item_batch_size_by_time items, ending a batch early when the next item
// (`item_bytes(index)`) would take its parameters past max_parameter_bytes or
// its results past max_result_bytes. An item over a budget alone gets a
// batch of its own. Empty for item_count 0.
[[nodiscard]] std::vector<ItemBatch> partition_items(
    std::size_t item_count, const ItemBatchPolicy& policy,
    const std::function<ItemBytes(std::size_t index)>& item_bytes);

// Whole seconds a batch of `count` items is estimated to take (at least 1),
// for TaskResources::est_seconds.
[[nodiscard]] std::uint64_t item_batch_estimated_seconds(const ItemBatchPolicy& policy,
                                                         std::uint64_t count);

}  // namespace svp::vision::tasks
