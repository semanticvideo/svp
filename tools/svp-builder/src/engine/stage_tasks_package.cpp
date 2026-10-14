#include "engine/frame_catalog_delta.hpp"
#include "engine/package_task_states.hpp"
#include "engine/stage_tasks.hpp"
#include "engine/vision_task_states.hpp"
#include "package_final_stages.hpp"

#include "svp/package/spatial_embedding_placeholders.hpp"
#include "svp/package/vision_lane_stages.hpp"

#include <filesystem>
#include <system_error>

namespace svp::builder::engine {
namespace {

nlohmann::json committed_manifest(const StageTaskEnvironment& environment) {
  return environment.results.json_state(stage_task_id(StageTaskKind::entities),
                                        package_state::kManifest);
}

PackageSkeletonStageResult committed_package_result(const StageTaskEnvironment& environment,
                                                    StageTaskKind producer) {
  return package_stage_result_from_json(
      environment.results.json_state(stage_task_id(producer),
                                     package_state::kPackageResult),
      environment.options);
}

std::uint64_t file_size_or_zero(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

}  // namespace

StageStates run_entities_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  const nlohmann::json manifest = make_package_manifest(
      environment.options, environment.plan_json.contains("canonical_analysis_raster")
                               ? environment.plan_json.at("canonical_analysis_raster")
                               : nlohmann::json());

  // The frames every stage decoded, replayed onto this task's plan copy.
  for (const StageTaskKind decoder : {StageTaskKind::color, StageTaskKind::canonical_frames,
                                      StageTaskKind::ocr, StageTaskKind::tracking}) {
    apply_frame_catalog_delta(
        task.context().frame_catalog,
        environment.results.json_state(stage_task_id(decoder), state_name::kFrameCatalog));
  }

  const auto vision = [&](StageTaskKind kind, const char* name) {
    return environment.results.json_state(stage_task_id(kind), name);
  };
  const svp::package::VisionLaneOutcome lane = svp::package::combine_vision_lane_results(
      environment.model_runtime_available,
      svp::package::vision_depth_stage_result_from_json(
          vision(StageTaskKind::depth, vision_state::kDepth)),
      svp::package::vision_ocr_stage_result_from_json(
          vision(StageTaskKind::ocr, vision_state::kOcr)),
      svp::package::vision_text_embedding_stage_result_from_json(
          vision(StageTaskKind::text_embeddings, vision_state::kTextEmbeddings)),
      svp::package::vision_tracking_stage_result_from_json(
          vision(StageTaskKind::tracking, vision_state::kTracking)));

  const PackageEntitiesStageResult entities =
      run_package_entities_stage(task.context(), lane.processor_records);

  const nlohmann::json foundation = {
      {"spatial_embedding_placeholders",
       svp::package::spatial_embedding_placeholder_summary_to_json(lane.summary)},
      {"frame_catalog_total_frames", entities.frame_catalog_total_frames},
      {"entity_artifacts", entities.entity_artifacts}};
  StageStates states;
  states[state_name::kFoundation] = json_state_bytes(foundation);
  states[package_state::kManifest] = json_state_bytes(manifest);
  return states;
}

StageStates run_relationships_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  const nlohmann::json summary = run_package_relationships_stage(task.context());
  StageStates states;
  states[state_name::kFoundation] =
      json_state_bytes({{"package_relationships_provenance", summary}});
  return states;
}

StageStates run_index_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  run_package_index_stage(task.context(), committed_manifest(environment));
  return {};
}

StageStates run_validation_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  const PackageSkeletonStageResult result =
      run_package_validation_stage(task.context(), committed_manifest(environment));
  StageStates states;
  states[package_state::kPackageResult] =
      json_state_bytes(package_stage_result_to_json(result));
  return states;
}

StageStates run_package_write_task(const StageTaskEnvironment& environment) {
  StageTaskContext task(environment);
  const PackageSkeletonStageResult result = run_package_write_stage(
      task.context(), committed_manifest(environment),
      committed_package_result(environment, StageTaskKind::validation));
  StageStates states;
  states[package_state::kPackageResult] =
      json_state_bytes(package_stage_result_to_json(result));
  states[package_state::kPublished] = json_state_bytes(
      {{"bytes", result.package_written ? file_size_or_zero(result.package_path) : 0}});
  return states;
}

}  // namespace svp::builder::engine
