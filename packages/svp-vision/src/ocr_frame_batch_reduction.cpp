#include "svp/vision/ocr_frame_batch_reduction.hpp"

#include <optional>
#include <string>
#include <utility>

namespace svp::vision {
namespace {

[[noreturn]] void fail(const std::string& message) {
  throw OcrFrameBatchReductionError("ocr frame batch reduction: " + message);
}

std::string ordinal_text(std::uint64_t ordinal) {
  return "sample ordinal " + std::to_string(ordinal);
}

}  // namespace

std::vector<OcrSampleDetections> assemble_ocr_frame_batches(
    const OcrSamplePlan& plan,
    std::vector<std::vector<OcrSampleDetections>> batch_results) {
  std::vector<std::optional<OcrSampleDetections>> slots(plan.samples.size());
  for (std::vector<OcrSampleDetections>& batch : batch_results) {
    for (OcrSampleDetections& record : batch) {
      const std::uint64_t ordinal = record.sample_ordinal;
      if (ordinal >= slots.size()) {
        fail(ordinal_text(ordinal) + " is outside the plan of " +
             std::to_string(slots.size()) + " samples");
      }
      const OcrSample& planned = plan.samples[ordinal];
      if (record.timestamp_us != planned.timestamp_us) {
        fail(ordinal_text(ordinal) + " has timestamp " +
             std::to_string(record.timestamp_us) + " us, plan has " +
             std::to_string(planned.timestamp_us) + " us");
      }
      if (record.status == OcrSampleStatus::not_started) {
        fail(ordinal_text(ordinal) + " never started; the OCR stage must run without batches");
      }
      if (record.status != OcrSampleStatus::decode_missed &&
          (record.frame_width != plan.frame_width ||
           record.frame_height != plan.frame_height)) {
        fail(ordinal_text(ordinal) + " was decoded at " +
             std::to_string(record.frame_width) + "x" +
             std::to_string(record.frame_height) + ", plan decodes at " +
             std::to_string(plan.frame_width) + "x" +
             std::to_string(plan.frame_height));
      }
      std::optional<OcrSampleDetections>& slot = slots[ordinal];
      if (slot.has_value()) fail(ordinal_text(ordinal) + " appears twice");
      slot = std::move(record);
    }
  }

  std::vector<OcrSampleDetections> ordered;
  ordered.reserve(slots.size());
  for (std::size_t ordinal = 0; ordinal < slots.size(); ++ordinal) {
    if (!slots[ordinal].has_value()) {
      fail(ordinal_text(ordinal) + " has no result (gap)");
    }
    ordered.push_back(std::move(*slots[ordinal]));
  }
  return ordered;
}

}  // namespace svp::vision
