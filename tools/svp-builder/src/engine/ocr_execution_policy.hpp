#pragma once

// Where and how a build runs its OCR frame-batch tasks.

#include "svp/exec/scheduler_policy.hpp"

#include <cstddef>

namespace svp::builder::engine {

// A build that is not --distributed runs its frame batches one at a time in
// its own process, exactly as the OCR stage always ran them: one PP-OCR
// session, with the recognition workers and ONNX Runtime threads of the
// build's thread plan doing the parallel work inside each frame. So a local
// build keeps its behaviour, memory peak, and speed (plan §7.1: local use
// unchanged). Only a --distributed build sizes this Mac's OCR slots from its
// measured capacity (DistributedFleet::coordinator_ocr_slots).
inline constexpr std::size_t kLocalOnlyOcrBatchSlots = 1;

// Scheduler rules for ocr.frame_batch tasks: svp-exec's defaults for small
// tasks that may run on another Mac (lease_policy.hpp, retry_policy.hpp):
// a lease that notices a lost worker within the 30 s floor, a hard deadline
// for a stuck attempt, and up to three attempts, preferring another
// executor. Unlike a whole stage, a batch writes nothing to staging, so
// running it again elsewhere is always safe.
[[nodiscard]] svp::exec::TaskTypePolicy ocr_frame_batch_task_policy();

}  // namespace svp::builder::engine
