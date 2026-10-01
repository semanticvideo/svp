#include "svp/vision/ocr_sample_plan.hpp"

#include <utility>

namespace svp::vision {

OcrSamplePlan make_ocr_sample_plan(OcrTemporalSamplingResult temporal_sampling,
                                   int frame_width,
                                   int frame_height) {
  OcrSamplePlan plan;
  plan.frame_width = frame_width;
  plan.frame_height = frame_height;
  plan.samples.reserve(temporal_sampling.timestamps_us.size());
  for (std::size_t index = 0; index < temporal_sampling.timestamps_us.size();
       ++index) {
    plan.samples.push_back(OcrSample{
        .ordinal = static_cast<std::uint64_t>(index),
        .timestamp_us = temporal_sampling.timestamps_us[index],
    });
  }
  plan.temporal_sampling = std::move(temporal_sampling);
  return plan;
}

}  // namespace svp::vision
