#pragma once

// What the coordinator assembles from committed task results once the graph
// has completed (whether the tasks ran now or were restored on --resume).

#include "build_pipeline_internal.hpp"
#include "engine/committed_stage_results.hpp"
#include "engine/stage_task_plan.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace svp::builder::engine {

// The builder foundation JSON: every task's foundation fragment merged in
// graph order (the inventory task's fragment is the ingest plan).
[[nodiscard]] nlohmann::json assemble_foundation_json(
    const std::vector<PlannedStageTask>& tasks, const CommittedStageResults& results);

// The package write task's result (package builds only).
[[nodiscard]] PackageSkeletonStageResult committed_package_result(
    const CommittedStageResults& results, const BuildPipelineOptions& options);

// When the publishing task (package write, or SVPI write) was committed by an
// earlier run, the file it published must still be there with the size it
// recorded; nullopt when it is, otherwise why not.
[[nodiscard]] std::optional<std::string> published_output_problem(
    const std::vector<PlannedStageTask>& tasks, const CommittedStageResults& results,
    const BuildPipelineOptions& options);

}  // namespace svp::builder::engine
