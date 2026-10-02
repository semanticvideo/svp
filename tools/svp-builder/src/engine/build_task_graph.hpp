#pragma once

// A build's task graph: the whole-stage graph (stage_task_plan.hpp) with the
// subtasks of every stage that runs split spliced in. A split stage's
// subtasks depend on what the stage task depended on and are placed just
// before it; the stage task (now the reducer) also depends on all of them.
// Stage order, dependencies, and concurrency between stages are unchanged.

#include "engine/ocr_frame_batch_plan.hpp"
#include "engine/stage_task_plan.hpp"
#include "engine/tracking_window_plan.hpp"

#include "svp/exec/task_graph.hpp"

#include <string>
#include <vector>

namespace svp::builder::engine {

// The stages of one build that run as subtasks; null for a stage that runs
// whole.
struct SplitStageTasks {
  const OcrFrameBatchPlan* ocr_batches = nullptr;
  const TrackingWindowPlan* tracking_windows = nullptr;
};

[[nodiscard]] svp::exec::TaskGraph make_split_build_task_graph(
    const std::vector<PlannedStageTask>& tasks, const std::string& build_session_id,
    const std::string& build_inputs_blake3, const SplitStageTasks& split);

}  // namespace svp::builder::engine
