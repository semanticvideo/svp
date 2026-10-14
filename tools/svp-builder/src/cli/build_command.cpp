#include "cli_context.hpp"
#include "cli_completion.hpp"
#include "build_selected_output.hpp"
#include "cli_run_telemetry.hpp"

#include "svp/builder/build_pipeline.hpp"

#include "distributed_cli.hpp"
#include "build_interrupt.hpp"
#include "resize/resized_source.hpp"

#include "svp/exec/cancellation_token.hpp"

#include <chrono>
#include <memory>
#include <iostream>
#include <optional>
#include <utility>

int run_build_command(const BuildCliOptions& requested, CLI::App* build_subcommand) {
  const std::optional<svp::builder::BuildStage> parsed_stage =
      svp::builder::parse_build_stage(requested.stop_after);
  if (!parsed_stage.has_value()) {
    std::cerr << "svp-builder build currently supports --stop-after media-ingest, audio, "
                 "vision-plan, foundation-color, foundation-ocr, or package\n";
    return 2;
  }
  if (requested.stop_after == "package-skeleton") {
    std::cerr << "warning: --stop-after package-skeleton is deprecated; use --stop-after package\n";
  }

  auto render_sink = resolve_cli_render_sink(
      requested.progress_mode, requested.quiet, build_subcommand);
  if (!render_sink) return 2;
  CliRunTelemetry telemetry("build", std::move(render_sink),
                            requested.run_report_path, requested.runtime_tools);
  auto progress_sink = telemetry.progress_sink();

  // --max-resolution (resize/resized_source.hpp): the build reads and describes
  // the resized media instead of the source, for every output format.
  BuildCliOptions options = requested;
  std::optional<svp::builder::resize::ResizeOutcome> resized;
  if (requested.max_resolution) {
    svp::exec::CancellationToken cancellation;
    const svp::builder::BuildInterruptScope interrupt_scope(cancellation);
    resized = svp::builder::resize::resize_source(
        {.source_path = requested.source_path,
         .probe_json_path = requested.probe_json_path,
         .output_path = requested.output_path,
         .max_resolution = *requested.max_resolution,
         .ffmpeg_path = requested.ffmpeg_path,
         .ffprobe_path = requested.ffprobe_path,
         .resume = requested.resume,
         .overwrite = requested.fresh || requested.overwrite,
         .cancellation = &cancellation},
        *progress_sink, requested.quiet);
    if (resized->cancelled) {
      return telemetry.finish(svp::builder::resize::resize_cancelled_exit_code());
    }
    if (!resized->error.empty()) {
      std::cerr << "svp-builder build: " << resized->error << "\n";
      return telemetry.finish(svp::builder::kBuildFailedExitCode);
    }
    if (resized->geometry) {
      options.source_path = resized->media_path.string();
      options.probe_json_path.clear();
    }
  }
  // An .svp package or an embedded SVPI carries the resized media, so its
  // separate file is removed once that output is written.
  const auto finish_resized = [&resized, &options](bool output_written) {
    if (resized) {
      svp::builder::resize::finish_resized_media(*resized, options.output_format != "svpi",
                                                 output_written);
    }
  };

  const auto started_at = std::chrono::steady_clock::now();
  // --distributed: the paired workers this build may use, for every output
  // format (the .svp, SVPI, and embedded SVPI builds run the same pipeline).
  const std::optional<std::shared_ptr<svp::builder::DistributedExecution>> fleet =
      make_cli_distributed_execution(options.distributed, options.require_workers,
                                     options.quiet, "build");
  if (!fleet) return 2;
  const std::shared_ptr<svp::builder::DistributedExecution>& distributed = *fleet;
  if (options.output_format != "svp") {
    const int exit_code =
        telemetry.finish(run_selected_output_build(options, progress_sink, distributed));
    finish_resized(exit_code == 0);
    if (exit_code == 0) {
      std::cout << format_cli_completion(
                       build_artifact_label(options.output_format), "created",
                       options.output_path,
                       std::chrono::steady_clock::now() - started_at)
                << "\n";
    }
    return exit_code;
  }

  svp::builder::BuildPipelineOptions pipeline_options;
  pipeline_options.source_path = options.source_path;
  pipeline_options.probe_json_path = options.probe_json_path;
  pipeline_options.ffprobe_path = options.ffprobe_path;
  pipeline_options.ffmpeg_path = options.ffmpeg_path;
  pipeline_options.output_path = options.output_path;
  pipeline_options.staging_dir = options.staging_dir;
  pipeline_options.model_cache_dir = options.model_cache_dir;
  pipeline_options.stop_after = *parsed_stage;
  pipeline_options.performance = options.performance;
  pipeline_options.visual_tracking_quality = options.visual_tracking_quality;
  pipeline_options.sherpa_lib_path = options.sherpa_lib_path;
  pipeline_options.allow_fallback_diarization = options.allow_fallback_diarization;
  pipeline_options.force_single_speaker = options.force_single_speaker;
  pipeline_options.serial_pipeline = options.serial_pipeline;
  pipeline_options.journal_mode =
      journal_mode_from_flags(options.resume, options.fresh);
  pipeline_options.progress_sink = progress_sink;
  pipeline_options.runtime_tools = options.runtime_tools;
  pipeline_options.quiet = options.quiet;
  pipeline_options.verbose = options.verbose;
  pipeline_options.distributed = distributed;

  const svp::builder::BuildPipelineResult result =
      svp::builder::BuildPipeline{}.run(pipeline_options);
  telemetry.record_thread_plan(result.thread_plan);
  const int exit_code = telemetry.finish(result.exit_code);
  finish_resized(exit_code == 0 &&
                 *parsed_stage == svp::builder::BuildStage::package_skeleton);
  if (exit_code == 0 &&
      *parsed_stage == svp::builder::BuildStage::package_skeleton) {
    std::cout << format_cli_completion(
                     build_artifact_label(options.output_format), "created",
                     options.output_path,
                     std::chrono::steady_clock::now() - started_at)
              << "\n";
  }
  return exit_code;
}
