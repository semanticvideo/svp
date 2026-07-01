#include "cli_context.hpp"

#include "svp/builder/progress_renderer.hpp"

#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <optional>

std::shared_ptr<svp::builder::BuildProgressSink> resolve_cli_progress_sink(
    const std::string& mode,
    bool quiet,
    CLI::App* subcommand) {
  auto progress_opt = subcommand->get_option("--progress");
  const bool progress_explicitly_set =
      progress_opt && progress_opt->count() > 0;

  std::optional<svp::builder::ProgressMode> resolved_mode;
  if (quiet) {
    if (progress_explicitly_set && mode == "json") {
      resolved_mode = svp::builder::ProgressMode::json;
    } else {
      resolved_mode = svp::builder::ProgressMode::none;
    }
  } else {
    resolved_mode = svp::builder::parse_progress_mode(mode);
  }

  if (!resolved_mode) {
    std::cerr << "svp-builder: invalid --progress value: " << mode << "\n";
    return nullptr;
  }

  const bool stderr_is_tty = isatty(fileno(stderr)) != 0;
  return svp::builder::make_progress_sink(
      *resolved_mode, std::cerr, stderr_is_tty);
}
