#include "cli_context.hpp"

#include "svp/builder/progress_renderer.hpp"
#include "svp/builder/progress_timeline.hpp"
#include "svp/progress/original_stderr.hpp"

#include <unistd.h>

#include <iostream>
#include <optional>
#include <utility>

std::shared_ptr<svp::builder::BuildProgressSink> resolve_cli_render_sink(
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

  // Progress goes to the stderr captured at startup, never to whatever fd 2
  // points at right now: stages silence native library logs by briefly
  // redirecting fd 2 to /dev/null while the other lane keeps emitting events.
  svp::progress::FdStream& progress_stream = svp::progress::original_stderr();
  const bool stderr_is_tty = isatty(progress_stream.fd()) != 0;
  return svp::builder::make_progress_sink(
      *resolved_mode, progress_stream.stream(), stderr_is_tty,
      progress_stream.fd());
}

std::shared_ptr<svp::builder::BuildProgressSink> resolve_cli_progress_sink(
    const std::string& mode,
    bool quiet,
    CLI::App* subcommand) {
  auto render_sink = resolve_cli_render_sink(mode, quiet, subcommand);
  if (!render_sink) return nullptr;
  return svp::builder::make_timestamped_progress_sink({std::move(render_sink)});
}
