#include "cli_context.hpp"
#include "cli_completion.hpp"
#include "build_selected_output.hpp"
#include "cli_run_telemetry.hpp"

#include "svp/builder/build_pipeline.hpp"

#include "distributed_cli.hpp"
#include "build_interrupt.hpp"
#include "resize/resized_source.hpp"
#include "batch/video_placement.hpp"
#include "batch_coordinators_cli.hpp"

#include "svp/builder/remote_video_builder.hpp"
#include "svp/builder/video_build_parameters.hpp"

#include "svp/exec/cancellation_token.hpp"
#if defined(__APPLE__)
#include "svp/exec/worker/local_load.hpp"
#endif

#include <chrono>
#include <filesystem>
#include <string>
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
         .remote_pieces = make_cli_remote_piece_encoders(
             requested.distributed, requested.require_workers, requested.ffmpeg_path),
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

  // --coordinators (batch/video_placement.hpp): while another build on this
  // Mac coordinates a video, this one goes to the first free Mac listed, still
  // distributed, and its output and run report come back here.
#if defined(__APPLE__)
  // Declared before the slot, so it outlives it.
  std::unique_ptr<svp::exec::worker::LocalLoadRecorder> coordinator_load;
#endif
  std::optional<svp::builder::batch::CoordinatorSlot> coordinator_slot;
  if (!options.coordinators.empty()) {
    const std::chrono::steady_clock::time_point placed_from = std::chrono::steady_clock::now();
    if (*parsed_stage != svp::builder::BuildStage::package_skeleton) {
      std::cerr << "svp-builder build: --coordinators needs the complete package stage\n";
      return telemetry.finish(2);
    }
    if (std::filesystem::exists(options.output_path) && !options.overwrite) {
      std::cerr << "output already exists; pass --overwrite to replace it\n";
      return telemetry.finish(svp::builder::kBuildFailedExitCode);
    }
    const std::optional<svp::builder::batch::VideoOutputFormat> format =
        svp::builder::batch::parse_video_output_format(options.output_format);
    std::optional<CliBatchCoordinators> coordinators = make_cli_batch_coordinators(
        options.coordinators, options.performance, options.model_cache_dir, options.ffmpeg_path,
        options.quiet, "build");
    if (!format || !coordinators) {
      return telemetry.finish(2);
    }
    svp::builder::batch::VideoBuildParameters parameters = coordinators->parameters;
    parameters.output_format = *format;
    parameters.source_name = std::filesystem::path(options.source_path).filename().string();
    parameters.performance = options.performance;
    parameters.visual_tracking_quality = options.visual_tracking_quality;
    parameters.allow_fallback_diarization = options.allow_fallback_diarization;
    parameters.force_single_speaker = options.force_single_speaker;
    parameters.serial_pipeline = options.serial_pipeline;
    parameters.distributed = options.distributed;
    parameters.require_workers = options.require_workers;
    parameters.run_report = !options.run_report_path.empty();
    svp::exec::CancellationToken cancellation;
    const svp::builder::BuildInterruptScope interrupt_scope(cancellation);
    svp::builder::batch::VideoPlacement placed = svp::builder::batch::place_video(
        {.slot_path = svp::builder::batch::default_coordinator_slot_path(),
         .coordinators = std::move(coordinators->macs),
         .request = {.item_id = "video",
                     .parameters = parameters,
                     .source_path = options.source_path,
                     .output_path = options.output_path,
                     .run_report_path = options.run_report_path},
         .busy_backoff = svp::builder::batch::kRemoteVideoBusyBackoff,
         .cancellation = &cancellation,
         .report = [quiet = options.quiet](const std::string& line) {
           if (!quiet) {
             std::cerr << "svp-builder build: " << line << "\n";
           }
         }});
    using Where = svp::builder::batch::VideoPlacement::Where;
    if (placed.where == Where::cancelled) {
      return telemetry.finish(svp::builder::resize::resize_cancelled_exit_code());
    }
    if (placed.where == Where::other_mac) {
      const bool built = placed.outcome.status == svp::builder::batch::RemoteVideoStatus::built;
      if (!options.quiet) {
        std::cerr << "svp-builder build: coordinated by " << placed.mac << ": "
                  << (built ? std::string("built") : "failed: " + placed.outcome.message)
                  << "\n";
      }
      if (built) {
        telemetry.leave_report_to_other_mac();
      }
      const int exit_code = telemetry.finish(built ? 0 : svp::builder::kBuildFailedExitCode);
      finish_resized(exit_code == 0);
      if (exit_code == 0) {
        std::cout << format_cli_completion(build_artifact_label(options.output_format),
                                           "created", options.output_path,
                                           std::chrono::steady_clock::now() - placed_from)
                  << "\n";
      }
      return exit_code;
    }
    coordinator_slot = std::move(placed.slot);
#if defined(__APPLE__)
    // This Mac's worker agent must see that it coordinates a video, so it takes
    // no whole-video job from another Mac while this build runs
    // (svp/exec/worker/local_load.hpp). A distributed build records it too once
    // its workers are prepared; a plain local build otherwise records nothing.
    coordinator_load = std::make_unique<svp::exec::worker::LocalLoadRecorder>();
    coordinator_load->add(svp::exec::worker::kCoordinatingTaskType);
#endif
  }

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
        telemetry.finish(run_selected_output_build(options, requested.source_path,
                                                   progress_sink, distributed));
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
  pipeline_options.capture_source_path = requested.source_path;
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
