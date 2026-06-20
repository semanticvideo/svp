#pragma once

#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/color_observation_records.hpp"

#include <nlohmann/json.hpp>

namespace svp::vision {

struct FoundationColorStagingArtifact {
  ColorObservationRecordPlan records;
  nlohmann::json processor_provenance;
  nlohmann::json manifest;
};

[[nodiscard]] ColorFrameSamplingInput build_foundation_color_staging_sample_input();
[[nodiscard]] FoundationColorStagingArtifact build_foundation_color_staging_artifact();
[[nodiscard]] nlohmann::json foundation_color_staging_artifact_to_json(
    const FoundationColorStagingArtifact& artifact);

}  // namespace svp::vision
