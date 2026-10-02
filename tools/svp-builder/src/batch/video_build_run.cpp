#include "video_build_run.hpp"

#include "svp/builder/embedded_svpi_transport.hpp"
#include "svp/builder/interlace.hpp"
#include "svp/models/thread_plan.hpp"

#include <system_error>

namespace svp::builder::batch {
namespace {

std::optional<svp::models::ThreadPlan> given_plan(const VideoBuildRunOptions& options) {
  if (!options.use_parameter_thread_plan) {
    return std::nullopt;
  }
  return svp::models::thread_plan_from_json(options.parameters.thread_plan);
}

InterlaceCreateOptions interlace_options(const VideoBuildRunOptions& options,
                                         const std::filesystem::path& output_path) {
  const VideoBuildParameters& parameters = options.parameters;
  InterlaceCreateOptions create;
  create.source_path = options.source_path.string();
  create.output_path = output_path.string();
  create.staging_dir = options.staging_dir.string();
  create.model_cache_dir = options.model_cache_dir.string();
  create.ffprobe_path = options.ffprobe_path;
  create.ffmpeg_path = options.ffmpeg_path;
  create.sherpa_lib_path = options.sherpa_lib_path;
  create.performance = parameters.performance;
  create.visual_tracking_quality = parameters.visual_tracking_quality;
  create.compute_full_blake3 = parameters.compute_full_blake3;
  create.core_only_diagnostic = parameters.core_only_diagnostic;
  create.allow_fallback_diarization = parameters.allow_fallback_diarization;
  create.force_single_speaker = parameters.force_single_speaker;
  create.serial_pipeline = parameters.serial_pipeline;
  create.progress_sink = options.progress_sink;
  create.distributed = options.distributed;
  create.thread_plan = given_plan(options);
  return create;
}

VideoBuildRunResult run_svp(const VideoBuildRunOptions& options) {
  const VideoBuildParameters& parameters = options.parameters;
  BuildPipelineOptions pipeline;
  pipeline.source_path = options.source_path.string();
  pipeline.ffprobe_path = options.ffprobe_path;
  pipeline.ffmpeg_path = options.ffmpeg_path;
  pipeline.output_path = options.output_path;
  pipeline.staging_dir = options.staging_dir;
  pipeline.model_cache_dir = options.model_cache_dir;
  pipeline.performance = parameters.performance;
  pipeline.visual_tracking_quality = parameters.visual_tracking_quality;
  pipeline.sherpa_lib_path = options.sherpa_lib_path;
  pipeline.allow_fallback_diarization = parameters.allow_fallback_diarization;
  pipeline.force_single_speaker = parameters.force_single_speaker;
  pipeline.serial_pipeline = parameters.serial_pipeline;
  pipeline.thread_plan = given_plan(options);
  pipeline.progress_sink = options.progress_sink;
  pipeline.runtime_tools = options.runtime_tools;
  pipeline.quiet = options.quiet;
  pipeline.verbose = options.verbose;
  // Each video's build is new: a batch re-run decides per output whether
  // to build at all (--resume / --fresh), not from per-video journals.
  pipeline.journal_mode = RecoveryJournalMode::fresh;
  pipeline.distributed = options.distributed;
  const BuildPipelineResult result = BuildPipeline{}.run(pipeline);
  return VideoBuildRunResult{
      .success = result.exit_code == 0,
      .error_message = result.exit_code == 0
                           ? std::string()
                           : (result.error_message.empty()
                                  ? "the build exited with status " +
                                        std::to_string(result.exit_code)
                                  : result.error_message),
      .cancelled = result.failure == BuildPipelineFailure::cancelled,
      .thread_plan = result.thread_plan};
}

}  // namespace

VideoBuildRunResult run_video_build(const VideoBuildRunOptions& options) {
  std::error_code error;
  if (std::filesystem::exists(options.output_path, error) && !options.overwrite_output) {
    return VideoBuildRunResult{.success = false,
                               .error_message = "output already exists: " +
                                                options.output_path.string()};
  }
  switch (options.parameters.output_format) {
    case VideoOutputFormat::svp:
      return run_svp(options);
    case VideoOutputFormat::svpi: {
      InterlaceCreateOptions create = interlace_options(options, options.output_path);
      create.journal_mode = RecoveryJournalMode::fresh;
      const InterlaceCreateResult result = interlace_create(create);
      return VideoBuildRunResult{
          .success = result.success,
          .error_message = result.error_message,
          .cancelled = result.pipeline_failure == BuildPipelineFailure::cancelled,
          .thread_plan = result.thread_plan};
    }
    case VideoOutputFormat::embedded_svpi: {
      EmbeddedTransportBuildOptions build;
      build.svpi_options = interlace_options(options, std::filesystem::path{});
      build.svpi_options.journal_mode = RecoveryJournalMode::fresh;
      build.output_path = options.output_path;
      build.overwrite_output = options.overwrite_output;
      const EmbeddedTransportBuildResult result = build_embedded_svpi_transport(build);
      return VideoBuildRunResult{.success = result.success,
                                 .error_message = result.error_message,
                                 .cancelled = false,
                                 .thread_plan = std::nullopt};
    }
  }
  return VideoBuildRunResult{.success = false, .error_message = "unknown output format"};
}

}  // namespace svp::builder::batch
