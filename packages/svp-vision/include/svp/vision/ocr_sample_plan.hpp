#pragma once

#include "svp/vision/ocr_temporal_sampling.hpp"

#include <cstdint>
#include <vector>

namespace svp::vision {

// One OCR sample: its position in the plan's ordered sample list and the
// source timestamp decoded for it. The ordinal is the canonical order key of
// every per-sample OCR result (plan §4.5): results are reduced by ordinal,
// never by the order in which work finished.
struct OcrSample {
  std::uint64_t ordinal = 0;
  std::int64_t timestamp_us = 0;

  bool operator==(const OcrSample&) const = default;
};

// The OCR sample schedule, computed once before any frame is decoded. Every
// frame batch, local or remote, decodes a slice of `samples` at
// frame_width x frame_height; the reducer checks that the batches cover the
// plan exactly.
struct OcrSamplePlan {
  OcrTemporalSamplingResult temporal_sampling;
  // samples[i].ordinal == i; timestamps follow temporal_sampling order.
  std::vector<OcrSample> samples;
  int frame_width = 0;
  int frame_height = 0;
};

[[nodiscard]] OcrSamplePlan make_ocr_sample_plan(
    OcrTemporalSamplingResult temporal_sampling,
    int frame_width,
    int frame_height);

}  // namespace svp::vision
