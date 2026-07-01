#include "cli_context.hpp"

#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/progress_renderer.hpp"

#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <optional>

int run_build_command(const BuildCliOptions& options, CLI::App* build_subcommand) {
  const std::optional<svp::builder::BuildStage> parsed_stage =
      svp::builder::parse_build_stage(options.stop_after);
  if (!parsed_stage.has_value()) {
    std::cerr << "svp-builder build currently supports --stop-after media-ingest, audio, "
                 "vision-plan, foundation-color, foundation-ocr, or package-skeleton\n";
    return 2;
  }

  auto progress_opt = build_subcommand->get_option("--progress");
  const bool progress_explicitly_set =
      progress_opt && progress_opt->count() > 0;

  std::optional<svp::builder::ProgressMode> resolved_mode;
  if (options.quiet) {
    if (progress_explicitly_set && options.progress_mode == "json") {
      resolved_mode = svp::builder::ProgressMode::json;
    } else {
      resolved_mode = svp::builder::ProgressMode::none;
    }
  } else {
    resolved_mode = svp::builder::parse_progress_mode(options.progress_mode);
  }

  if (!resolved_mode) {
    std::cerr << "svp-builder: invalid --progress value: "
              << options.progress_mode << "\n";
    return 2;
  }

  const bool stderr_is_tty = isatty(fileno(stderr)) != 0;
  auto progress_sink = svp::builder::make_progress_sink(
      *resolved_mode, std::cerr, stderr_is_tty);

  svp::builder::BuildPipelineOptions pipeline_options;
  pipeline_options.source_path = options.source_path;
  pipeline_options.probe_json_path = options.probe_json_path;
  pipeline_options.ffprobe_path = options.ffprobe_path;
  pipeline_options.ffmpeg_path = options.ffmpeg_path;
  pipeline_options.output_path = options.output_path;
  pipeline_options.staging_dir = options.staging_dir;
  pipeline_options.model_cache_dir = options.model_cache_dir;
  pipeline_options.stop_after = *parsed_stage;
  pipeline_options.sherpa_lib_path = options.sherpa_lib_path;
  pipeline_options.allow_fallback_diarization = options.allow_fallback_diarization;
  pipeline_options.force_single_speaker = options.force_single_speaker;
  pipeline_options.progress_sink = progress_sink;

  const svp::builder::BuildPipelineResult result =
      svp::builder::BuildPipeline{}.run(pipeline_options);
  return result.exit_code;
}
