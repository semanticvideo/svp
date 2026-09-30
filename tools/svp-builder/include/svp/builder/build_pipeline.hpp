#pragma once

#include "svp/builder/build_progress.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/inference_performance.hpp"
#include "svp/vision/visual_tracking_quality.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder {

enum class BuildStage {
  media_ingest,
  audio,
  vision_plan,
  foundation_color,
  foundation_ocr,
  package_skeleton,
};

struct BuildStageExecutionPlan {
  bool run_audio = false;
  bool run_vision_plan = false;
  bool run_foundation_color = false;
  bool run_foundation_ocr = false;
  bool run_package_skeleton = false;
};

struct BuilderConcurrencyPolicy {
  std::size_t single_video_heavy_lanes = 1;
  std::size_t max_batch_jobs = 1;
};

struct BuildOutputPaths {
  std::filesystem::path package_path;
  std::filesystem::path json_output_path;
};

struct BuildPipelineOptions {
  std::string source_path;
  std::string probe_json_path;
  std::string ffprobe_path = "ffprobe";
  std::string ffmpeg_path = "ffmpeg";
  std::filesystem::path output_path;
  std::filesystem::path staging_dir;
  std::filesystem::path model_cache_dir;
  BuildStage stop_after = BuildStage::package_skeleton;
  svp::vision::InferencePerformanceOptions performance;
  std::string visual_tracking_quality{
      svp::vision::kDefaultVisualTrackingQualityName};
  std::string sherpa_lib_path;
  bool allow_fallback_diarization = false;
  bool force_single_speaker = false;
  bool serial_pipeline = false;
  bool reset_staging_before_stages = false;
  // Runtime thread counts to use as given (a distributed coordinator's plan).
  // Empty: the build resolves its plan from the host once at start.
  std::optional<svp::models::ThreadPlan> thread_plan;
  std::shared_ptr<BuildProgressSink> progress_sink;
  bool quiet = false;
  bool verbose = false;
};

enum class BuildPipelineFailure {
  none,
  model_cache_preflight,
  processing,
  // The requested package artifact could not be written (for example the
  // output directory is missing or not writable).
  package_write,
};

// Exit status for a build that could not produce what was requested: a
// processing error or an artifact that could not be written. Validator
// verdicts on a package that was written are passed through unchanged instead.
inline constexpr int kBuildFailedExitCode = 1;

struct BuildPipelineResult {
  int exit_code = 0;
  BuildPipelineFailure failure = BuildPipelineFailure::none;
  std::string error_message;
  // The plan every stage ran with; empty if the build failed before
  // resolving it.
  std::optional<svp::models::ThreadPlanResolution> thread_plan;
};

std::vector<std::string_view> supported_build_stage_names();
std::optional<BuildStage> parse_build_stage(std::string_view value);
std::string_view build_stage_name(BuildStage stage);
BuildStageExecutionPlan execution_plan_for_stage(BuildStage stage);
BuilderConcurrencyPolicy builder_concurrency_policy(
    const svp::vision::InferencePerformanceOptions& performance,
    std::size_t requested_batch_jobs);
std::filesystem::path default_staging_dir_for_output(
    const std::filesystem::path& output_path);
BuildOutputPaths resolve_package_skeleton_output_paths(
    const std::filesystem::path& output_path);

class BuildPipeline {
 public:
  BuildPipelineResult run(const BuildPipelineOptions& options) const;
};

}  // namespace svp::builder
