#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace svp::vision {

// How the OCR sample plan is cut into ocr.frame_batch tasks (plan §4.4
// "dynamic balancing"). Batch size only changes how work is scheduled, never
// the output: every batch returns one record per sample and the reducer
// consumes them in sample-ordinal order, so any partition reduces to the same
// bytes (tested for sizes 1, 3, 8, all-in-one, and shuffled completion).
//
// Sizing is by time, not by a frame count: a batch holds as many samples as
// fit in target_task_seconds at the current per-sample cost estimate. Per
// frame cost is very uneven (plan §2.4 item 4: median 562 ms, p90 5.2 s, max
// 10.4 s on the Gator excerpt), so small batches let pull-based scheduling
// move text-heavy regions to idle workers and keep a lost lease cheap to
// retry, while per-task overhead (lease, framing, result commit) stays small
// relative to the work. A scheduler that observes real per-sample costs
// passes its own estimate; the default is only the prior.
struct OcrBatchPolicy {
  // Wall time one ocr.frame_batch task should take.
  double target_task_seconds = 10.0;
  // Prior cost of one sample (decode + PP-OCR det/rec) before any run has
  // been observed: the measured Gator excerpt median (plan §2.2, §2.4 item 4),
  // rounded up.
  double estimated_seconds_per_sample = 0.6;
};

// A contiguous run of sample ordinals [first_ordinal, first_ordinal + count).
struct OcrSampleBatch {
  std::uint64_t first_ordinal = 0;
  std::uint64_t count = 0;

  bool operator==(const OcrSampleBatch&) const = default;
};

// Throws std::invalid_argument unless both policy values are finite and > 0.
void validate_ocr_batch_policy(const OcrBatchPolicy& policy);

// Samples per batch: floor(target / estimate), at least one.
[[nodiscard]] std::uint64_t ocr_batch_sample_count(const OcrBatchPolicy& policy);

// Cuts [0, sample_count) into consecutive batches of ocr_batch_sample_count
// samples (the last may be shorter). Empty for sample_count == 0.
[[nodiscard]] std::vector<OcrSampleBatch> partition_ocr_samples(
    std::size_t sample_count,
    const OcrBatchPolicy& policy);

}  // namespace svp::vision
