#pragma once

#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/color_observation_records.hpp"

namespace svp::vision {

struct FoundationColorObservationOptions {
  std::string provenance_id = "processor_color_quantizer_0001";
  int first_observation_number = 1;
};

[[nodiscard]] ColorObservationRecordPlan build_foundation_color_observations(
    const ColorFrameSamplingInput& input,
    const FoundationColorObservationOptions& options = {});

}  // namespace svp::vision
