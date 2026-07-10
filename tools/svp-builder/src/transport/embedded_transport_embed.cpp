#include "svp/builder/embedded_svpi_transport.hpp"

#include "svp/builder/interlace.hpp"
#include "svp/core/path.hpp"
#include "svp/package/embedded_svpi.hpp"
#include "svp/validation/embedded_svpi_transport_validator.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/svpi_validator.hpp"

#include "replacement_binding_media.hpp"
#include "transport_output_policy.hpp"

#include <exception>

namespace svp::builder {

EmbeddedTransportEmbedResult embed_svpi_transport(
    const EmbeddedTransportEmbedOptions& options) {
  EmbeddedTransportEmbedResult result;
  result.output_path = options.output_path;
  auto sink = options.progress_sink
      ? options.progress_sink
      : default_progress_sink();
  if (!svp::core::has_extension(options.svpi_path, ".svpi")) {
    result.error_message = "Transport embed requires a canonical .svpi payload.";
    return result;
  }
  const auto container = svp::package::inspect_iso_bmff_container(
      options.container_path);
  if (!container.structure_valid || !container.supported) {
    result.error_message = container.diagnostic;
    return result;
  }
  if (!detail::transport_output_path_matches_container(
          options.output_path, container.kind, result.error_message)) {
    return result;
  }

  svp::validation::SvpiValidatorOptions svpi_options;
  svpi_options.validation_codes_path = options.validation_codes_path;
  const auto standalone_report =
      svp::validation::validate_svpi_package(options.svpi_path, svpi_options);
  if (svp::validation::exit_code(standalone_report) != 0) {
    result.validation_report = standalone_report;
    result.error_message = "Input SVPI package validation failed.";
    return result;
  }

  auto binding_media = detail::prepare_binding_media(
      options.container_path, options.replace_existing);
  if (!binding_media.success) {
    result.error_message = binding_media.error_message;
    return result;
  }

  InterlaceValidateOptions binding_options;
  binding_options.svpi_path = options.svpi_path.string();
  binding_options.media_path = binding_media.candidate.path.string();
  binding_options.ffprobe_path = options.ffprobe_path;
  binding_options.validation_codes_path = options.validation_codes_path.string();
  const auto binding = interlace_validate(binding_options);
  if (!binding.structure_valid || !binding.binding_attempted ||
      !binding.binding_verified) {
    result.validation_report = binding.validation_report;
    result.error_message = binding.error_message.empty()
        ? "SVPI media binding does not match the input ISO BMFF container."
        : binding.error_message;
    return result;
  }

  svp::package::EmbeddedSvpiWriteOptions write_options;
  write_options.replace_existing = options.replace_existing;
  write_options.overwrite_output = options.overwrite_output;
  sink->emit(make_stage_started(ProgressStageId::transport_embed));
  const auto embedded = svp::package::embed_svpi_in_iso_bmff(
      options.container_path, options.svpi_path, options.output_path,
      write_options);
  if (!embedded.success) {
    result.error_message = embedded.error_message;
    try {
      const auto registry = svp::validation::load_validation_code_registry(
          options.validation_codes_path);
      for (const auto& issue : embedded.inspection.issues) {
        svp::validation::add_finding(
            result.validation_report,
            svp::validation::make_finding(
                registry,
                svp::validation::validation_code_for_embedded_issue(issue.code),
                "/iso_bmff/offset/" + std::to_string(issue.offset), issue.message));
      }
      svp::validation::recompute_status(result.validation_report);
    } catch (const std::exception&) {
      // The primary operation error remains authoritative if registry loading fails.
    }
    sink->emit(make_stage_failed(
        ProgressStageId::transport_embed, result.error_message));
    return result;
  }
  sink->emit(make_artifact_written(
      ProgressStageId::transport_embed, options.output_path));
  sink->emit(make_stage_completed(ProgressStageId::transport_embed));

  sink->emit(make_stage_started(ProgressStageId::embedded_validate));
  svp::validation::EmbeddedSvpiTransportValidatorOptions validator_options;
  validator_options.validation_codes_path = options.validation_codes_path;
  result.validation_report = svp::validation::validate_embedded_svpi_transport(
      options.output_path, validator_options);
  if (svp::validation::exit_code(result.validation_report) != 0) {
    result.error_message = "Final embedded SVPI transport validation failed.";
    sink->emit(make_stage_failed(ProgressStageId::embedded_validate,
                                 result.error_message));
    return result;
  }
  sink->emit(make_stage_completed(ProgressStageId::embedded_validate));
  result.success = true;
  return result;
}

}  // namespace svp::builder
