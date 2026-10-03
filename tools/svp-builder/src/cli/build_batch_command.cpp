#include "batch_coordinators_cli.hpp"
#include "cli_context.hpp"
#include "cli_run_telemetry.hpp"
#include "distributed_cli.hpp"

#include "batch/build_batch.hpp"

#include <iostream>
#include <utility>

namespace {

using svp::builder::batch::BuildBatchResult;
using svp::builder::batch::BuildBatchStatus;

void render(const BuildBatchResult& result) {
  std::size_t created = 0;
  std::size_t kept = 0;
  std::size_t failed = 0;
  for (const auto& item : result.items) {
    created += item.status == BuildBatchStatus::created ? 1 : 0;
    kept += item.status == BuildBatchStatus::kept ? 1 : 0;
    failed += item.status == BuildBatchStatus::failed ||
                      item.status == BuildBatchStatus::cancelled
                  ? 1
                  : 0;
  }
  std::cout << "Batch build summary:\n"
            << "  created: " << created << "\n"
            << "  kept: " << kept << "\n"
            << "  failed: " << failed << "\n";
  for (const auto& item : result.items) {
    std::cout << "  " << item.source.filename().string() << ": "
              << svp::builder::batch::build_batch_status_label(item.status) << "\n"
              << "    output: " << item.artifact_path.string() << "\n";
    if (!item.built_on.empty()) {
      std::cout << "    built on: " << item.built_on << "\n";
    }
    if (!item.error_message.empty()) {
      std::cout << "    error: " << item.error_message << "\n";
    }
  }
  for (const auto& dropped : result.dropped_coordinators) {
    std::cout << "  coordinator not used: " << dropped << "\n";
  }
}

}  // namespace

int run_build_batch_command(const BuildBatchCliOptions& options, CLI::App* subcommand) {
  const std::optional<svp::builder::batch::VideoOutputFormat> format =
      svp::builder::batch::parse_video_output_format(options.output_format);
  if (!format) {
    std::cerr << "svp-builder build-batch: unknown --output-format " << options.output_format
              << "\n";
    return 2;
  }
  auto render_sink = resolve_cli_render_sink(options.progress_mode, options.quiet, subcommand);
  if (!render_sink) return 2;
  // Fails early, as `build` does, when --distributed cannot be used here.
  if (!make_cli_distributed_execution(options.distributed, options.require_workers,
                                      options.quiet, "build-batch")) {
    return 2;
  }
  std::optional<CliBatchCoordinators> coordinators = make_cli_batch_coordinators(
      options.coordinators, options.performance, options.model_cache_dir, options.ffmpeg_path,
      options.quiet, "build-batch");
  if (!coordinators) {
    return 2;
  }

  svp::builder::batch::BuildBatchOptions batch;
  for (const std::string& source : options.sources) {
    batch.sources.emplace_back(source);
  }
  batch.out_dir = options.out_dir;
  batch.parameters = coordinators->parameters;
  batch.parameters.output_format = *format;
  batch.parameters.performance = options.performance;
  batch.parameters.visual_tracking_quality = options.visual_tracking_quality;
  batch.parameters.allow_fallback_diarization = options.allow_fallback_diarization;
  batch.parameters.force_single_speaker = options.force_single_speaker;
  batch.parameters.serial_pipeline = options.serial_pipeline;
  batch.parameters.distributed = options.distributed;
  batch.parameters.require_workers = options.require_workers;
  batch.parameters.run_report = options.run_report;
  batch.staging_dir = options.staging_dir;
  batch.model_cache_dir = options.model_cache_dir;
  batch.ffprobe_path = options.ffprobe_path;
  batch.ffmpeg_path = options.ffmpeg_path;
  batch.sherpa_lib_path = options.sherpa_lib_path;
  batch.runtime_tools = options.runtime_tools;
  batch.existing = options.resume ? svp::builder::batch::ExistingOutputs::resume
                   : options.fresh || options.overwrite
                       ? svp::builder::batch::ExistingOutputs::rebuild
                       : svp::builder::batch::ExistingOutputs::refuse;
  if (options.distributed || options.require_workers > 0) {
    batch.make_distributed = [distributed = options.distributed,
                              require_workers = options.require_workers,
                              quiet = options.quiet]() {
      return make_cli_distributed_execution(distributed, require_workers, quiet, "build-batch")
          .value_or(nullptr);
    };
  }
  batch.coordinators = std::move(coordinators->macs);
  // A video built here is reported as `build` reports one.
  batch.run_local = [runtime_tools = options.runtime_tools](
                        const svp::builder::batch::VideoBuildRunOptions& run,
                        const std::filesystem::path& run_report_path) {
    CliRunTelemetry telemetry("build", run.progress_sink, run_report_path, runtime_tools);
    svp::builder::batch::VideoBuildRunOptions reported = run;
    reported.progress_sink = telemetry.progress_sink();
    svp::builder::batch::VideoBuildRunResult result =
        svp::builder::batch::run_video_build(reported);
    telemetry.record_thread_plan(result.thread_plan);
    if (telemetry.finish(result.success ? 0 : 1) != 0 && result.success) {
      result.success = false;
      result.error_message = "the run report could not be written: " + run_report_path.string();
    }
    return result;
  };
  batch.progress_sink = render_sink;
  batch.quiet = options.quiet;
  batch.verbose = options.verbose;

  const BuildBatchResult result = svp::builder::batch::build_batch(batch);
  render(result);
  return result.all_succeeded() ? 0 : 1;
}
