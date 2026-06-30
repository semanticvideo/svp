#include "svp/builder/interlace.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/package/package_summary.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/report_json.hpp"

#include <nlohmann/json.hpp>

#include <iostream>

namespace svp::builder {

InterlaceValidateResult interlace_validate(const InterlaceValidateOptions& options) {
  InterlaceValidateResult result;

  svp::validation::SvpiValidatorOptions validator_opts;
  validator_opts.validation_codes_path = options.validation_codes_path;

  result.validation_report = svp::validation::validate_svpi_package(
      options.svpi_path, validator_opts);

  result.structure_valid =
      (svp::validation::exit_code(result.validation_report) == 0);

  if (!options.media_path.empty()) {
    result.binding_attempted = true;

    auto layout_result = svp::package::read_package_layout(options.svpi_path);
    if (!layout_result.has_value()) {
      result.binding_state_label = "unavailable";
      result.binding_failing_checks.push_back("could not read SVPI layout");
      return result;
    }

    const auto& layout = layout_result.value();
    if (!layout.has_entry("media_binding.json")) {
      result.binding_state_label = "unavailable";
      result.binding_failing_checks.push_back("media_binding.json not found in SVPI");
      return result;
    }

    auto binding_entry = svp::package::read_package_entry(
        options.svpi_path, "media_binding.json");
    if (!binding_entry.has_value()) {
      result.binding_state_label = "unavailable";
      result.binding_failing_checks.push_back("could not read media_binding.json");
      return result;
    }

    auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
    auto verification = svp::package::verify_media_binding(
        options.media_path, binding_doc);

    result.binding_state_label = verification.state_label;
    result.binding_passing_checks = verification.passing_checks;
    result.binding_failing_checks = verification.failing_checks;
    result.binding_verified =
        (verification.state == svp::package::BindingVerificationState::verified);
  }

  return result;
}

}  // namespace svp::builder
