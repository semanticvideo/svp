#include "build_pipeline_internal.hpp"

#include "svp/vision/observation_pipeline_plan.hpp"

namespace svp::builder {

void run_vision_plan_stage(BuildPipelineContext& context) {
  const svp::vision::VisionObservationPipelinePlan vision_plan =
      svp::vision::build_vision_observation_pipeline_plan(context.plan);
  context.output["vision_observation_pipeline"] =
      svp::vision::vision_observation_pipeline_plan_to_json(vision_plan);
}

}  // namespace svp::builder
