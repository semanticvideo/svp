#pragma once

// How one video of a batch is built, on whichever Mac coordinates it: the
// batch Mac for its own share of the videos, or another Mac's worker service
// for a whole-video job (video_build_parameters.hpp). Both call this, so a
// video's package never depends on where it was built: the same options
// reach the same pipeline as `svp-builder build --output-format <format>`
// (.svp: the build pipeline; SVPI: interlace create; embedded SVPI: the
// embedded transport build).

#include "svp/builder/video_build_parameters.hpp"

#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/distributed_execution.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace svp::builder::batch {

// What the Mac running the build supplies: its own paths and tools, never
// the other Mac's.
struct VideoBuildRunOptions {
  VideoBuildParameters parameters;
  std::filesystem::path source_path;
  std::filesystem::path output_path;
  // Empty: the build's default beside the output.
  std::filesystem::path staging_dir;
  std::filesystem::path model_cache_dir;
  std::string ffprobe_path = "ffprobe";
  std::string ffmpeg_path = "ffmpeg";
  std::string sherpa_lib_path;
  std::optional<RuntimeToolSelection> runtime_tools;
  // Replace an existing output (the batch decided it may).
  bool overwrite_output = false;
  // Use parameters.thread_plan as given (a whole-video job); otherwise the
  // plan is resolved on this Mac, as `build` does.
  bool use_parameter_thread_plan = false;
  std::shared_ptr<DistributedExecution> distributed;
  std::shared_ptr<BuildProgressSink> progress_sink;
  bool quiet = false;
  bool verbose = false;
};

struct VideoBuildRunResult {
  bool success = false;
  std::string error_message;
  // A cancelled build (Ctrl-C, SIGTERM): the batch stops.
  bool cancelled = false;
  std::optional<svp::models::ThreadPlanResolution> thread_plan;
};

[[nodiscard]] VideoBuildRunResult run_video_build(const VideoBuildRunOptions& options);

}  // namespace svp::builder::batch
