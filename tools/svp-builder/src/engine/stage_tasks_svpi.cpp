#include "engine/package_task_states.hpp"
#include "engine/stage_tasks.hpp"
#include "interlace/svpi_publication.hpp"
#include "package_final_stages.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"

#include <filesystem>
#include <system_error>

namespace svp::builder::engine {
namespace {

constexpr const char* kMediaBindingState = "media_binding";
constexpr const char* kSvpiResultState = "svpi_result";

// The exit status the package build alone would have ended with: the SVPI
// write falls back to core-only content when that is non-zero and staging
// holds no semantic sections, as interlace create always has.
int package_exit_code(const PackageSkeletonStageResult& package) {
  if (!package.package_written) {
    return kBuildFailedExitCode;
  }
  return package.validator_passes ? 0 : package.validator_exit_code;
}

}  // namespace

StageStates run_media_binding_task(const StageTaskEnvironment& environment) {
  const SvpiPublicationOptions& svpi = environment.options.svpi.value();
  environment.progress_sink.emit(make_stage_started(ProgressStageId::media_binding));
  svp::package::MediaBindingFactoryOptions binding_opts;
  binding_opts.ffprobe_path = environment.options.ffprobe_path;
  binding_opts.compute_full_blake3 = svpi.compute_full_blake3;
  binding_opts.compute_chunk_proof = svpi.compute_chunk_proof;
  const svp::package::MediaBindingDocument binding = svp::package::create_media_binding(
      std::filesystem::path(environment.options.source_path), binding_opts);
  environment.progress_sink.emit(make_stage_completed(ProgressStageId::media_binding));
  StageStates states;
  states[kMediaBindingState] = json_state_bytes(svp::package::to_json(binding));
  return states;
}

StageStates run_svpi_write_task(const StageTaskEnvironment& environment) {
  const SvpiPublicationOptions& svpi = environment.options.svpi.value();
  const svp::package::MediaBindingDocument binding =
      svp::package::media_binding_document_from_json(environment.results.json_state(
          stage_task_id(StageTaskKind::media_binding), kMediaBindingState));
  const PackageSkeletonStageResult package = package_stage_result_from_json(
      environment.results.json_state(stage_task_id(StageTaskKind::package_write),
                                     package_state::kPackageResult),
      environment.options);
  const std::string blake3_state =
      svp::package::to_string(binding.bindings.at(0).identity.blake3_state);

  // The internal package is not part of the sidecar.
  const std::filesystem::path temp_package = environment.options.output_path;
  std::error_code ignored;
  std::filesystem::remove(temp_package, ignored);
  std::filesystem::remove(temp_package.string() + ".json", ignored);

  const SvpiWriteRequest request{.source_path = environment.options.source_path,
                                 .output_path = svpi.svpi_path.string()};
  const nlohmann::json sections = detect_section_states(environment.staging_dir);
  const bool semantic_content = has_semantic_content(sections);
  InterlaceCreateResult published;
  if (package_exit_code(package) != 0 && !semantic_content) {
    std::filesystem::remove_all(environment.staging_dir);
    published = write_core_only_svpi(request, binding, blake3_state, "blocked",
                                     kSvpiBlockedNotes, environment.staging_dir,
                                     environment.progress_sink);
  } else {
    published = write_svpi_from_staging(request, binding, blake3_state,
                                        environment.staging_dir, sections,
                                        svpi_provenance_notes(semantic_content),
                                        environment.progress_sink);
  }

  std::uint64_t svpi_bytes = 0;
  if (published.success) {
    const auto size = std::filesystem::file_size(svpi.svpi_path, ignored);
    svpi_bytes = ignored ? 0 : static_cast<std::uint64_t>(size);
  }
  StageStates states;
  states[kSvpiResultState] = json_state_bytes({{"success", published.success},
                                               {"error_message", published.error_message},
                                               {"blake3_state", blake3_state},
                                               {"binding_state", published.binding_state}});
  states[package_state::kPublished] = json_state_bytes({{"bytes", svpi_bytes}});
  return states;
}

SvpiPublicationResult svpi_publication_result(const CommittedStageResults& results) {
  const nlohmann::json state =
      results.json_state(stage_task_id(StageTaskKind::svpi_write), kSvpiResultState);
  return SvpiPublicationResult{
      .success = state.at("success").get<bool>(),
      .error_message = state.at("error_message").get<std::string>(),
      .blake3_state = state.at("blake3_state").get<std::string>(),
      .binding_state = state.at("binding_state").get<std::string>()};
}

}  // namespace svp::builder::engine
