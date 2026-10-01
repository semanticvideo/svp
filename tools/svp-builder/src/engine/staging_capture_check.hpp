#pragma once

// End-of-build check that the recovery journal could reproduce the staging
// directory: every staging entry lies inside some task's scope and holds what
// the last task covering it captured. A failure means a stage wrote outside
// its declared scope, so resuming an interrupted build of this kind could
// differ from an uninterrupted one. The finished build itself is unaffected.

#include "engine/committed_stage_results.hpp"
#include "engine/stage_task_plan.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace svp::builder::engine {

// One line per staging entry the journal would not reproduce; empty when the
// journal reproduces staging exactly.
[[nodiscard]] std::vector<std::string> unreproducible_staging_entries(
    const std::filesystem::path& staging_dir, const std::vector<PlannedStageTask>& tasks,
    const CommittedStageResults& results);

}  // namespace svp::builder::engine
