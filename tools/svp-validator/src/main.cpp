#include "svp/core/version.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/validation/embedded_svpi_transport_validator.hpp"
#include "svp/validation/report_json.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/validator.hpp"

#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

namespace {

void print_human_report(const svp::validation::ValidationReport& report) {
  std::cout << "SVP validation: " << svp::validation::to_string(report.status)
            << "\n";
  std::cout << "Core: " << svp::validation::to_string(report.core_status)
            << "\n";
  std::cout << "Authenticity: "
            << svp::validation::to_string(report.authenticity_status) << "\n";
  if (report.embedding_transport.present) {
    const auto& transport = report.embedding_transport;
    std::cout << "Embedding transport: " << transport.status << "\n";
    std::cout << "  container_kind: " << transport.container_kind << "\n";
    std::cout << "  major_brand: " << transport.major_brand << "\n";
    std::cout << "  container_supported: "
              << (transport.container_supported ? "yes" : "no") << "\n";
    std::cout << "  embedding_detected: "
              << (transport.embedding_detected ? "yes" : "no") << "\n";
    std::cout << "Embedded package: "
              << transport.embedded_package_status << "\n";
    std::cout << "  profile_version: " << transport.profile_version << "\n";
    std::cout << "  uuid: " << transport.uuid << "\n";
    std::cout << "  box_offset: " << transport.box_offset << "\n";
    std::cout << "  box_size: " << transport.box_size << "\n";
    std::cout << "  payload_offset: " << transport.payload_offset << "\n";
    std::cout << "  payload_size: " << transport.payload_size << "\n";
    std::cout << "  payload_hash: " << transport.payload_hash_status << "\n";
  }

  const auto print_bucket = [](std::string_view label,
                               const std::vector<svp::validation::ValidationFinding>& findings) {
    for (const auto& finding : findings) {
      std::cout << label << " " << finding.code;
      if (!finding.path.empty()) {
        std::cout << " " << finding.path;
      }
      std::cout << ": " << finding.message << "\n";
    }
  };

  print_bucket("error", report.errors);
  print_bucket("warning", report.warnings);
  print_bucket("info", report.infos);
  print_bucket("authenticity", report.authenticity);

  if (report.errors.empty() && report.warnings.empty() && report.infos.empty() &&
      report.authenticity.empty()) {
    std::cout << "No findings.\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"SVP, SVPI, and Embedded SVPI Transport validator"};
  app.set_version_flag("--version",
                       svp::core::tool_version_label("svp-validator"));
  app.require_subcommand(0, 1);

  std::string package_path;
  bool json_output = false;
  std::string validation_codes_path;

  auto* validate = app.add_subcommand(
      "validate", "Validate an SVP, SVPI, or Embedded SVPI Transport");
  validate->add_option(
      "package", package_path,
      "Path to an SVP, SVPI, or ISO BMFF media container")->required();
  validate->add_flag("--json", json_output, "Emit a machine-readable validation report");
  validate->add_option("--validation-codes", validation_codes_path,
                       "Path to the validation-code registry");

  CLI11_PARSE(app, argc, argv);

  if (*validate) {
    const auto probe = svp::package::probe_package(package_path);
    const auto report = (probe.iso_bmff.signature_present ||
                         (!probe.has_svp_extension &&
                          !probe.has_svpi_extension))
        ? svp::validation::validate_embedded_svpi_transport(
              package_path,
              svp::validation::EmbeddedSvpiTransportValidatorOptions{
                  .validation_codes_path = validation_codes_path,
              })
        : probe.has_svpi_extension
              ? svp::validation::validate_svpi_package(
                    package_path,
                    svp::validation::SvpiValidatorOptions{
                        .validation_codes_path = validation_codes_path,
                    })
              : svp::validation::validate_package(
                    package_path,
                    svp::validation::ValidatorOptions{
                        .validation_codes_path = validation_codes_path,
                    });

    if (json_output) {
      nlohmann::json json = report;
      std::cout << json.dump(2) << "\n";
    } else {
      print_human_report(report);
    }

    return svp::validation::exit_code(report);
  }

  return 0;
}
