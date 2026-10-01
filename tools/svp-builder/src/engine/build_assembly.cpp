#include "engine/build_assembly.hpp"

#include "engine/package_task_states.hpp"
#include "engine/stage_task_environment.hpp"
#include "package_final_stages.hpp"

#include <filesystem>
#include <system_error>

namespace svp::builder::engine {

nlohmann::json assemble_foundation_json(const std::vector<PlannedStageTask>& tasks,
                                        const CommittedStageResults& results) {
  nlohmann::json output = nlohmann::json::object();
  for (const PlannedStageTask& task : tasks) {
    const std::string_view task_id = stage_task_id(task.kind);
    if (!results.has_state(task_id, state_name::kFoundation)) {
      continue;
    }
    const nlohmann::json fragment = results.json_state(task_id, state_name::kFoundation);
    for (const auto& [key, value] : fragment.items()) {
      output[key] = value;
    }
  }
  return output;
}

PackageSkeletonStageResult committed_package_result(const CommittedStageResults& results,
                                                    const BuildPipelineOptions& options) {
  return package_stage_result_from_json(
      results.json_state(stage_task_id(StageTaskKind::package_write),
                         package_state::kPackageResult),
      options);
}

std::optional<std::string> published_output_problem(
    const std::vector<PlannedStageTask>& tasks, const CommittedStageResults& results,
    const BuildPipelineOptions& options) {
  for (const PlannedStageTask& task : tasks) {
    const std::string_view task_id = stage_task_id(task.kind);
    if (!results.has_state(task_id, package_state::kPublished)) {
      continue;
    }
    const auto bytes =
        results.json_state(task_id, package_state::kPublished).at("bytes").get<std::uint64_t>();
    if (bytes == 0) {
      continue;  // nothing was published
    }
    // The last publishing task in the graph owns the published artifact.
    const std::filesystem::path published =
        task.kind == StageTaskKind::svpi_write
            ? options.svpi.value().svpi_path
            : resolve_package_skeleton_output_paths(options.output_path).package_path;
    if (task.kind == StageTaskKind::package_write && options.svpi) {
      continue;  // interlace create's internal package is not an output
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(published, error);
    if (error || static_cast<std::uint64_t>(size) != bytes) {
      return published.string() + " was published by " + std::string(task_id) +
             " but is missing or has changed since";
    }
  }
  return std::nullopt;
}

std::optional<std::string> unfinished_publication(
    const BuildStageExecutionPlan& stage_plan,
    const PackageSkeletonStageResult& package_result,
    const std::optional<SvpiPublicationResult>& svpi) {
  if (!stage_plan.run_package_skeleton) {
    return std::nullopt;
  }
  if (svpi) {
    if (!svpi->success) {
      return "the SVPI was not written" +
             (svpi->error_message.empty() ? std::string() : " (" + svpi->error_message + ")");
    }
    if (!svpi->validator_passed) {
      return "the SVPI failed strict validation";
    }
    return std::nullopt;
  }
  if (!package_result.package_written) {
    return "the package was not written";
  }
  if (!package_result.validator_passes) {
    return "the package failed strict validation";
  }
  return std::nullopt;
}

std::string kept_journal_note(const std::string& reason,
                              const std::filesystem::path& journal_root) {
  return reason + ", so the recovery journal is kept at " + journal_root.string() +
         " (RC2 section 20.5.1); the next build of this output needs --resume (continues "
         "from the journal without rerunning completed stages) or --fresh (discards it "
         "and rebuilds)";
}

}  // namespace svp::builder::engine
