#include "svp/vision/foundation_color_observations.hpp"

namespace svp::vision {

ColorObservationRecordPlan build_foundation_color_observations(
    const ColorFrameSamplingInput& input,
    const FoundationColorObservationOptions& options) {
  const SampledColorObservationPlan sampled =
      sample_frame_scene_shot_colors(input);
  return plan_color_observation_records(
      sampled.summaries,
      sampled.target_references,
      ColorObservationConstructionOptions{
          options.provenance_id,
          "color_obs_",
          options.first_observation_number,
      });
}

}  // namespace svp::vision
