#include "interlace_batch_internal.hpp"

#include "svp/builder/embedded_svpi_transport.hpp"
#include "svp/package/media_binding.hpp"
#include "svp/package/embedded_svpi.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/validation/embedded_svpi_transport_validator.hpp"
#include "svp/validation/report_json.hpp"

#include <utility>

namespace svp::builder {

EmbeddedBatchArtifactState inspect_embedded_batch_artifact(
    const std::filesystem::path& artifact_path,
    const std::filesystem::path& source_path,
    const std::filesystem::path& validation_codes_path,
    std::string& error_message) {
  const auto inspection = svp::package::inspect_embedded_svpi(
      artifact_path, false);
  if (inspection.container_structure_valid && inspection.container.supported &&
      inspection.embeddings.empty()) {
    return EmbeddedBatchArtifactState::absent;
  }
  if (check_embedded_batch_artifact(
          artifact_path, source_path, validation_codes_path, error_message)) {
    return EmbeddedBatchArtifactState::valid;
  }
  return EmbeddedBatchArtifactState::invalid;
}

bool check_embedded_batch_artifact(
    const std::filesystem::path& artifact_path,
    const std::filesystem::path& source_path,
    const std::filesystem::path& validation_codes_path,
    std::string& error_message) {
  svp::validation::EmbeddedSvpiTransportValidatorOptions validation_options;
  validation_options.validation_codes_path = validation_codes_path;
  const auto report = svp::validation::validate_embedded_svpi_transport(
      artifact_path, validation_options);
  if (svp::validation::exit_code(report) != 0) {
    error_message = "Embedded SVPI Transport validation failed.";
    for (const auto& finding : report.errors) {
      error_message += "\n  " + finding.code + ": " + finding.message;
    }
    return false;
  }

  std::error_code equivalence_error;
  const bool artifact_is_source = std::filesystem::equivalent(
      artifact_path, source_path, equivalence_error);
  if (!equivalence_error && artifact_is_source) {
    return true;
  }

  const auto binding_entry = svp::package::read_package_entry(
      artifact_path, "media_binding.json");
  if (!binding_entry.has_value()) {
    error_message = "Could not read embedded media_binding.json.";
    return false;
  }

  const auto binding = svp::package::parse_media_binding_json(
      binding_entry.value());
  const auto verification = svp::package::verify_media_binding(
      source_path, binding);
  if (verification.state !=
      svp::package::BindingVerificationState::verified) {
    error_message = "Embedded SVPI binding mismatch: " +
                    verification.state_label;
    return false;
  }
  return true;
}

bool create_embedded_batch_artifact(
    const BatchCreateOptions& options,
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_path,
    const std::string& staging_dir,
    std::string& error_message,
    std::string& blake3_state,
    const std::shared_ptr<BuildProgressSink>& progress_sink,
    bool overwrite_output) {
  InterlaceCreateOptions create;
  create.source_path = source_path.string();
  create.staging_dir = staging_dir;
  create.model_cache_dir = options.model_cache_dir;
  create.ffprobe_path = options.ffprobe_path;
  create.ffmpeg_path = options.ffmpeg_path;
  create.sherpa_lib_path = options.sherpa_lib_path;
  create.performance = options.performance;
  create.core_only_diagnostic = options.core_only_diagnostic;
  create.allow_fallback_diarization = options.allow_fallback_diarization;
  create.force_single_speaker = options.force_single_speaker;
  create.serial_pipeline = options.serial_pipeline;
  create.progress_sink = progress_sink;

  EmbeddedTransportBuildOptions build;
  build.svpi_options = std::move(create);
  build.output_path = output_path;
  build.overwrite_output = overwrite_output;
  const auto result = build_embedded_svpi_transport(build);
  if (!result.success) {
    error_message = result.error_message;
    return false;
  }

  blake3_state = "present";
  return true;
}

}  // namespace svp::builder
