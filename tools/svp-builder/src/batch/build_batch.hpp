#pragma once

// `svp-builder build-batch` (M6): build many videos, each as `svp-builder
// build --output-format <format>` would, spread over this Mac and the other
// Macs named with --coordinators (batch_dispatch.hpp). Every video's package
// lands in --out-dir: <stem>.svp, <stem>.svpi, or (embedded SVPI) the
// source's own file name.
//
// An output that already exists:
//   * by default the video fails, as `build` refuses to replace an output;
//   * --resume keeps it when it is complete and verified for its source
//     (the package validates and its media binding or embedded original is
//     that source) and builds it again otherwise;
//   * --fresh (or --overwrite) builds every video again and replaces its
//     output.

#include "batch_dispatch.hpp"
#include "video_build_run.hpp"

#include "svp/builder/build_progress.hpp"
#include "svp/builder/distributed_execution.hpp"
#include "svp/builder/remote_video_builder.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace svp::builder::batch {

enum class ExistingOutputs { refuse, resume, rebuild };

struct BuildBatchOptions {
  std::vector<std::filesystem::path> sources;
  std::filesystem::path out_dir;
  // Everything that shapes each package. thread_plan and ffmpeg_build are
  // this Mac's, filled for other Macs; source_name is set per video.
  VideoBuildParameters parameters;
  // Per-video staging lives in <staging_dir>/<stem>.staging when set.
  std::filesystem::path staging_dir;
  std::filesystem::path model_cache_dir;
  std::string ffprobe_path = "ffprobe";
  std::string ffmpeg_path = "ffmpeg";
  std::string sherpa_lib_path;
  std::optional<RuntimeToolSelection> runtime_tools;
  ExistingOutputs existing = ExistingOutputs::refuse;
  // --distributed on this Mac: the paired workers for one video's build.
  std::function<std::shared_ptr<DistributedExecution>()> make_distributed;
  // --coordinators: the other Macs.
  std::vector<std::shared_ptr<RemoteVideoBuilder>> coordinators;
  // Runs one video on this Mac; the CLI wraps run_video_build with the run
  // report (--run-report). Empty: run_video_build.
  std::function<VideoBuildRunResult(const VideoBuildRunOptions& options,
                                    const std::filesystem::path& run_report_path)>
      run_local;
  std::shared_ptr<BuildProgressSink> progress_sink;
  bool quiet = false;
  bool verbose = false;
};

enum class BuildBatchStatus { created, kept, failed, cancelled };

[[nodiscard]] std::string_view build_batch_status_label(BuildBatchStatus status) noexcept;

struct BuildBatchItemResult {
  std::filesystem::path source;
  std::filesystem::path artifact_path;
  std::filesystem::path run_report_path;
  BuildBatchStatus status = BuildBatchStatus::failed;
  // "this Mac" or the other Mac's name.
  std::string built_on;
  std::string error_message;
};

struct BuildBatchResult {
  std::vector<BuildBatchItemResult> items;
  // Other Macs the batch stopped using, and why.
  std::vector<std::string> dropped_coordinators;

  [[nodiscard]] bool all_succeeded() const noexcept;
};

// Where build-batch writes the package of `source` in `out_dir`.
[[nodiscard]] std::filesystem::path build_batch_artifact_path(
    const std::filesystem::path& source, const std::filesystem::path& out_dir,
    VideoOutputFormat format);

[[nodiscard]] BuildBatchResult build_batch(const BuildBatchOptions& options);

}  // namespace svp::builder::batch
