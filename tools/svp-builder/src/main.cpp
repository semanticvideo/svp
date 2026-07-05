#include "cli/cli_context.hpp"
#include "staging_cleanup.hpp"
#include "svp/core/version.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
  svp::builder::install_staging_interrupt_cleanup();

  CLI::App app{"SVP builder"};
  app.set_version_flag("--version", svp::core::tool_version_label("svp-builder"));
  app.require_subcommand(0, 1);

  CliContext context;
  register_cli(app, context);

  CLI11_PARSE(app, argc, argv);

  try {
    return run_selected_command(context);
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return 1;
  }
}
