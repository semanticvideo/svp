#include "svp/core/version.hpp"

#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

namespace {

void run_validate_stub(const std::string& package_path) {
  std::cout << "validate is a Phase 01 stub";
  if (!package_path.empty()) {
    std::cout << " for " << package_path;
  }
  std::cout << ". No package validation was performed.\n";
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"SVP package validator"};
  app.set_version_flag("--version",
                       svp::core::tool_version_label("svp-validator"));
  app.require_subcommand(0, 1);

  std::string package_path;
  auto* validate = app.add_subcommand("validate", "Placeholder for package validation");
  validate->add_option("package", package_path, "Path to a .svp package");
  validate->callback([&package_path]() { run_validate_stub(package_path); });

  CLI11_PARSE(app, argc, argv);

  return 0;
}
