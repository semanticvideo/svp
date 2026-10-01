#pragma once

#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/ocr_sample_plan.hpp"

#include <stdexcept>
#include <vector>

namespace svp::vision {

// Frame batch results that do not cover the sample plan exactly.
class OcrFrameBatchReductionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// First step of the OCR reducer (plan §2.4 item 3): concatenates frame batch
// results by sample ordinal. Batches may arrive in any order and be of any
// size; the result is the plan's samples in ordinal order, each exactly once,
// so everything downstream (frame registration, reconciliation, record IDs)
// sees the same input whatever the partition or completion order.
//
// Throws OcrFrameBatchReductionError, naming the first offending ordinal, for
// a gap (a planned sample with no record), a duplicate, an ordinal outside the
// plan, a timestamp that differs from the plan, or a decoded frame whose size
// differs from the plan's decode size. Never returns a partial reduction.
[[nodiscard]] std::vector<OcrSampleDetections> assemble_ocr_frame_batches(
    const OcrSamplePlan& plan,
    std::vector<std::vector<OcrSampleDetections>> batch_results);

}  // namespace svp::vision
