#pragma once

// Registers the build's whole-stage task types (plan §4.3: a runtime runs only
// the task types compiled into its registry). Each type runs its stage
// function in this process, then captures the task's planned staging scope
// and states as the task's outputs (stage_task_products.hpp).

#include "engine/stage_output_access.hpp"
#include "engine/stage_task_environment.hpp"
#include "engine/stage_task_plan.hpp"

#include "svp/exec/task_registry.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder::engine {

// Error code of a task that ended the build with a stage exit status.
inline constexpr const char* kStageExitStatusErrorCode = "stage_exit_status";

// The exit status a stage asked for (StageExitError), if any. Thread-safe.
class StageExitRecord {
 public:
  void record(int exit_code);
  [[nodiscard]] std::optional<int> exit_code() const;

 private:
  mutable std::mutex mutex_;
  std::optional<int> exit_code_;
};

void register_stage_task_types(svp::exec::TaskTypeRegistry& registry,
                               const std::vector<PlannedStageTask>& tasks,
                               const StageTaskEnvironment& environment,
                               StageOutputAccess& outputs, StageExitRecord& exits);

}  // namespace svp::builder::engine
