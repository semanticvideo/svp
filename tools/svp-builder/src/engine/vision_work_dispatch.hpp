#pragma once

// The dispatchers a --distributed build gives its vision stages
// (svp/vision/dispatched_work.hpp, vision_dispatch_setup.hpp). Each one, when
// its stage reaches its per-item work:
//   1. cuts the items into tasks by the type's batch policy (measured
//      seconds per item, frame-size byte budgets; item_batch_policy.hpp);
//   2. runs them on this Mac's slots and the workers (run_subtasks);
//   3. returns the outcomes in item order.
// A dispatcher returns nullopt, so the stage does its work itself as a local
// build does, when its task type is not dispatched in this build or the
// stage's model or PP-OCR settings are not the ones the workers were given.

#include "engine/vision_dispatch_setup.hpp"

#include "svp/exec/task_artifact_access.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/package/vision_lane_stages.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <memory>
#include <vector>

namespace svp::builder::engine {

// `registry` holds the dispatched task types for this Mac's slots and
// `artifacts` resolves the source for them; both must outlive the returned
// hooks.
[[nodiscard]] svp::package::VisionWorkDispatch make_vision_work_dispatch(
    std::shared_ptr<const VisionDispatchSetup> setup,
    const svp::exec::TaskTypeRegistry& registry, svp::exec::TaskArtifactAccess& artifacts);

}  // namespace svp::builder::engine
