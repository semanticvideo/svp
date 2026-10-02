#include "batch_coordinators_cli.hpp"

#if defined(__APPLE__)
#include "batch/paired_video_builder.hpp"
#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_thread_plan.hpp"
#include "svp/core/memory_diagnostics.hpp"
#include "svp/models/cache.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#endif

#include <iostream>

void add_coordinators_option(CLI::App& command, std::vector<std::string>& coordinators) {
  command
      .add_option("--coordinators", coordinators,
                  "Other Macs (pairing ids or user@host of this Mac's paired workers, comma "
                  "separated) that may each coordinate one video of the batch at a time; their "
                  "worker service must run as a LaunchDaemon (workers pair --system-service)")
      ->delimiter(',');
}

std::optional<CliBatchCoordinators> make_cli_batch_coordinators(
    const std::vector<std::string>& coordinators,
    const svp::vision::InferencePerformanceOptions& performance,
    const std::filesystem::path& model_cache_dir, const std::string& ffmpeg_path, bool quiet,
    std::string_view command_name) {
  CliBatchCoordinators result;
  if (coordinators.empty()) {
    return result;
  }
#if defined(__APPLE__)
  try {
    // The plan a build of these options resolves on this Mac: every other
    // Mac builds with it, as this Mac's own videos do.
    svp::builder::BuildPipelineOptions plan_options;
    plan_options.performance = performance;
    const svp::models::ThreadPlan plan =
        svp::builder::resolve_build_thread_plan(plan_options,
                                                svp::models::detect_host_cpu_topology(),
                                                svp::models::process_environment_lookup(),
                                                svp::core::memory_diagnostics_enabled())
            .plan;
    const std::filesystem::path cache =
        model_cache_dir.empty() ? svp::models::model_cache_root() : model_cache_dir;
    const auto supplies = svp::builder::batch::prepare_video_build_supplies(cache, plan);
    result.macs = svp::builder::batch::paired_video_builders(coordinators, supplies);
    result.parameters.thread_plan = svp::models::thread_plan_to_json(plan);
    result.parameters.ffmpeg_build =
        svp::vision::tasks::cached_ffmpeg_build_identity(ffmpeg_path).value_or(std::string());
    if (!quiet) {
      for (const auto& mac : result.macs) {
        std::cerr << "svp-builder " << command_name << ": coordinator " << mac->name() << "\n";
      }
    }
    return result;
  } catch (const std::exception& error) {
    std::cerr << "svp-builder " << command_name << ": --coordinators: " << error.what() << "\n";
    return std::nullopt;
  }
#else
  (void)performance;
  (void)model_cache_dir;
  (void)ffmpeg_path;
  (void)quiet;
  std::cerr << "svp-builder " << command_name << ": --coordinators needs macOS (paired workers)\n";
  return std::nullopt;
#endif
}
