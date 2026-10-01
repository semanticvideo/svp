#include "engine/frame_catalog_delta.hpp"
#include "engine/stage_tasks.hpp"

namespace svp::builder::engine {
namespace {

StageStates foundation_states(StageTaskContext& task) {
  StageStates states;
  states[state_name::kFoundation] = json_state_bytes(task.output());
  states[state_name::kFrameCatalog] =
      json_state_bytes(frame_catalog_delta(task.context().frame_catalog));
  return states;
}

}  // namespace

StageStates run_inventory_task(const StageTaskEnvironment& environment) {
  // The media probe and ingest plan are made by the planner (every schedule
  // is computed from them); this task records the plan as the foundation
  // JSON's base.
  StageStates states;
  states[state_name::kFoundation] = json_state_bytes(environment.plan_json);
  return states;
}

StageStates run_color_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  emit_stage_started(task.context(), ProgressStageId::color);
  run_foundation_color_stage(task.context());
  emit_stage_completed(task.context(), ProgressStageId::color);
  return foundation_states(task);
}

StageStates run_vision_plan_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  emit_stage_started(task.context(), ProgressStageId::vision_plan);
  run_vision_plan_stage(task.context());
  emit_stage_completed(task.context(), ProgressStageId::vision_plan);
  return foundation_states(task);
}

StageStates run_foundation_ocr_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  emit_stage_started(task.context(), ProgressStageId::ocr);
  run_foundation_ocr_stage(task.context());
  emit_stage_completed(task.context(), ProgressStageId::ocr);
  return foundation_states(task);
}

}  // namespace svp::builder::engine
