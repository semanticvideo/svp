#pragma once

// Where and how a --distributed build runs its tracking window tasks.

#include "svp/exec/scheduler_policy.hpp"

#include <cstddef>

namespace svp::builder::engine {

// Window tasks this Mac runs at once when its tracking capacity could not be
// measured: one, as the tracking stage always ran its windows (one after
// another, with the thread plan's ONNX Runtime threads doing the parallel
// work inside each window). A measured capacity
// (DistributedFleet::coordinator_tracking_slots) replaces it.
inline constexpr std::size_t kCoordinatorTrackWindowSlotsWithoutMeasurement = 1;

// Scheduler rules for track.window tasks: svp-exec's lease defaults for
// tasks that may run on another Mac (lease_policy.hpp): a lease that notices
// a lost worker within the 30 s floor and a hard deadline for a stuck
// attempt (at least 10 min, the plan's longest measured unit being a 49 s
// window). Attempts: svp-exec's default budget for transient losses, plus
// one for each worker executor, because a worker refuses to return a window
// that went wrong there (track_window_task.hpp, window_failed_here) and
// retries prefer an executor that has not failed the task: so a window that
// goes wrong on every worker still reaches this Mac, which records it as a
// local build does. A window writes nothing to staging, so running it again
// elsewhere is always safe.
[[nodiscard]] svp::exec::TaskTypePolicy track_window_task_policy(std::size_t worker_executors);

}  // namespace svp::builder::engine
