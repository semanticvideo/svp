#pragma once

// Runs one stage's dispatched tasks (vision_dispatch_setup.hpp) to completion
// on this Mac's in-process slots for their type and on the workers'
// executors, with the scheduler's leases, retries, and quarantine (plan
// §4.4): a worker that is lost or killed mid-task has its tasks retried
// elsewhere, this Mac included. Results come back in the tasks' canonical
// order, whichever executor ran them and in whatever order they finished.

#include "engine/vision_dispatch_setup.hpp"

#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/task_artifact_access.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_registry.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace svp::builder::engine {

struct SubtaskRunRequest {
  std::string task_type;
  // In canonical order; no node depends on another.
  std::vector<svp::exec::TaskNode> nodes;
  // Called on the scheduler thread as each task commits, with how many items
  // it carried.
  std::function<void(const svp::exec::TaskNode& node)> on_committed;
  // Files the tasks read that the workers were not given when the build
  // prepared them (the staged analysis audio): each worker session is
  // supplied them before its leases are sent. This Mac's slots resolve them
  // through the caller's artifact access.
  std::vector<DispatchedInput> inputs;
};

// The committed results, in `request.nodes` order. Throws
// svp::vision::DispatchedWorkError when a task fails for good, the run is
// cancelled, or there is no executor for the type.
[[nodiscard]] std::vector<svp::exec::CommittedResult> run_subtasks(
    const SubtaskRunRequest& request, const VisionDispatchSetup& setup,
    const svp::exec::TaskTypeRegistry& registry, svp::exec::TaskArtifactAccess& artifacts);

}  // namespace svp::builder::engine
