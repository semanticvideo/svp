#include "svp/core/version.hpp"
#include "svp/validation/report_json.hpp"
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
  CLI::App app{"SVP package validator"};
  app.set_version_flag("--version",
                       svp::core::tool_version_label("svp-validator"));
  app.require_subcommand(0, 1);

  std::string package_path;
  bool json_output = false;
  std::string validation_codes_path = "spec/registries/validation-codes.json";

  auto* validate = app.add_subcommand("validate", "Validate an SVP package");
  validate->add_option("package", package_path, "Path to a .svp package")->required();
  validate->add_flag("--json", json_output, "Emit a machine-readable validation report");
  validate->add_option("--validation-codes", validation_codes_path,
                       "Path to the validation-code registry");

  CLI11_PARSE(app, argc, argv);

  if (*validate) {
    const auto report = svp::validation::validate_package(
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
