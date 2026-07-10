#include "svp/builder/embedded_svpi_transport.hpp"

#include "svp/package/embedded_svpi.hpp"
#include "svp/core/path.hpp"
#include "svp/validation/svpi_validator.hpp"

namespace svp::builder {

EmbeddedTransportExtractResult extract_svpi_transport(
    const EmbeddedTransportExtractOptions& options) {
  EmbeddedTransportExtractResult result;
  result.output_path = options.output_path;
  if (!svp::core::has_extension(options.output_path, ".svpi")) {
    result.error_message = "Transport extract requires a .svpi output path.";
    return result;
  }
  const auto extracted = svp::package::extract_embedded_svpi(
      options.container_path, options.output_path, options.overwrite_output);
  if (!extracted.success) {
    result.error_message = extracted.error_message;
    return result;
  }

  svp::validation::SvpiValidatorOptions validator_options;
  validator_options.validation_codes_path = options.validation_codes_path;
  result.validation_report = svp::validation::validate_svpi_package(
      options.output_path, validator_options);
  result.package_valid = svp::validation::exit_code(result.validation_report) == 0;
  result.success = true;
  if (!result.package_valid) {
    result.error_message =
        "SVPI bytes were recovered exactly, but package validation reported errors.";
  }
  return result;
}

}  // namespace svp::builder
