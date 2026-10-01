#pragma once

#include "svp/builder/build_progress.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/builder/runtime_tools.hpp"
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

// What a build does with an existing RC2 §20.4 recovery journal for its
// output (`<output>-journal/`).
enum class RecoveryJournalMode {
  // Start a new journal. An existing journal is neither reused nor deleted:
  // the build fails and asks for --resume or --fresh, so an interrupted
  // build's work is never discarded silently.
  require_new,
  // Continue from the existing journal (`--resume`) after verifying the
  // source fingerprint and every completed artifact.
  resume,
  // Delete any existing journal and start over (`--fresh`).
  fresh,
};

// interlace create: after the package, bind the source media and write the
// SVPI sidecar as tasks of the same build.
struct SvpiPublicationOptions {
  std::filesystem::path svpi_path;
  bool compute_full_blake3 = true;
  bool compute_chunk_proof = true;
};

struct SvpiPublicationResult {
  bool success = false;
  bool validator_passed = false;
  std::string error_message;
  std::string blake3_state;
  std::string binding_state;
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
  // Where ffmpeg/ffprobe/sherpa-onnx came from; recorded in the builder
  // foundation JSON (`builder_command.runtime_tools`) when set.
  std::optional<RuntimeToolSelection> runtime_tools;
  bool quiet = false;
  bool verbose = false;
  RecoveryJournalMode journal_mode = RecoveryJournalMode::require_new;
  // Output path the recovery journal belongs to; empty means output_path.
  // interlace create journals next to the .svpi it publishes, not next to its
  // internal package.
  std::filesystem::path journal_output_path;
  std::optional<SvpiPublicationOptions> svpi;
};

enum class BuildPipelineFailure {
  none,
  model_cache_preflight,
  processing,
  // The requested package artifact could not be written (for example the
  // output directory is missing or not writable).
  package_write,
  // A recovery journal blocked the build: one exists without --resume or
  // --fresh, none exists for --resume, it is locked by another build, or it
  // was recorded for a different source or different options.
  recovery_journal,
  // Ctrl-C or SIGTERM; the recovery journal is kept for --resume.
  cancelled,
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
  // Set when options.svpi was given and the build reached the SVPI write.
  std::optional<SvpiPublicationResult> svpi;
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
