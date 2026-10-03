#pragma once

// The whole-video job (M6, task type video.build): a batch hands one video to
// another Mac, whose worker service coordinates that video's build there,
// --distributed when the batch is, with the batch Mac's runtime, model
// bundles, and thread plan, and returns the package.
//
// Inputs: `source` (the video, a BLAKE3-verified blob) and `model_lock` (the
// batch Mac's model-lock.json; the task's model_refs name every bundle it
// pins). Parameters carry the build options that shape the package, each
// from a closed set; no message carries a path, a command, or an
// environment variable (plan §4.3):
//
//   {"allow_fallback_diarization":bool, "compute_full_blake3":bool,
//    "core_only_diagnostic":bool, "distributed":bool,
//    "ffmpeg_build":"b3:<hex>"|"", "force_single_speaker":bool,
//    "ocr_performance_profile":"serial"|"background"|"conservative"|"fast",
//    "output_format":"svp"|"svpi"|"embedded-svpi", "require_workers":n,
//    "run_report":bool, "serial_pipeline":bool, "source_name":"<file name>",
//    "thread_plan":{ThreadPlan JSON},
//    "visual_tracking_quality":"off"|"low"|"medium"|"high"}
//
// `source_name` is the source's own file name, which packages record; the
// worker gives its copy of the source that name. `ffmpeg_build` is the batch
// Mac's ffmpeg build identity; a worker whose ffmpeg differs refuses the job,
// so the package cannot depend on which Mac built it. `compute_full_blake3`
// and `core_only_diagnostic` are SVPI options (interlace create-batch's
// --no-blake3 and --core-only-diagnostic).
//
// Outputs: the package record (canonical JSON {"blob":{"blake3":"<hex>",
// "bytes":n},"file_name":"<name>"}): the package stays in the worker's
// content-addressed cache, pinned while the job's session lasts, and the
// batch fetches it with BLOB_GET, verified by BLAKE3 (packages can be far
// larger than a result frame carries). With `run_report`, the build's run
// report JSON follows as a second output.

#include "svp/exec/task_spec.hpp"
#include "svp/vision/inference_performance.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace svp::builder::batch {

// Equal to svp::exec::worker::kCoordinatingTaskType (local_load.hpp), which
// the worker agent counts for a Mac that coordinates a video.
inline constexpr std::string_view kVideoBuildTaskType = "video.build";
inline constexpr std::uint64_t kVideoBuildTaskTypeVersion = 1;
inline constexpr std::string_view kVideoBuildSourceInput = "source";
inline constexpr std::string_view kVideoBuildModelLockInput = "model_lock";
inline constexpr std::string_view kVideoBuildPackageRole = "video_build_package";
inline constexpr std::string_view kVideoBuildRunReportRole = "video_build_run_report";

// Each Mac coordinates one video at a time (M6 design): its build already
// spreads over every Mac's measured slots, so a second video there would
// only compete with the first for the same slots.
inline constexpr std::uint64_t kVideoBuildSlotsPerMac = 1;

// Scheduling estimates the TaskSpec must carry (task_spec.hpp). Nothing
// sizes a whole-video job by them: the coordinating Mac admits it by its
// one declared video.build slot and by memory. There is no measured model of
// a whole build's peak memory, so none is claimed (0: admitted while free
// memory exceeds the worker's reserve, admission.hpp), and one thread is the
// smallest estimate the spec accepts.
inline constexpr std::uint64_t kVideoBuildEstimatedPeakRssMb = 0;
inline constexpr std::uint64_t kVideoBuildEstimatedCpuThreads = 1;

// A job that cannot run on that Mac (its ffmpeg or a model bundle differs, a
// file cannot be staged): the batch gives the video to another Mac. A build
// that ran and failed is reported with its own code and is not retried.
inline constexpr std::string_view kVideoBuildUnavailableCode = "video_build_unavailable";
inline constexpr std::string_view kVideoBuildFailedCode = "video_build_failed";

enum class VideoOutputFormat { svp, svpi, embedded_svpi };

[[nodiscard]] std::string_view video_output_format_name(VideoOutputFormat format) noexcept;
[[nodiscard]] std::optional<VideoOutputFormat> parse_video_output_format(
    std::string_view name) noexcept;

struct VideoBuildParameters {
  VideoOutputFormat output_format = VideoOutputFormat::svp;
  std::string source_name;
  svp::vision::InferencePerformanceOptions performance;
  std::string visual_tracking_quality;
  bool allow_fallback_diarization = false;
  bool force_single_speaker = false;
  bool serial_pipeline = false;
  bool compute_full_blake3 = true;
  bool core_only_diagnostic = false;
  bool distributed = false;
  std::uint64_t require_workers = 0;
  bool run_report = false;
  std::string ffmpeg_build;
  nlohmann::json thread_plan = nlohmann::json::object();
};

// Equal when their JSON forms are equal.
[[nodiscard]] bool operator==(const VideoBuildParameters& left, const VideoBuildParameters& right);

[[nodiscard]] nlohmann::json video_build_parameters_to_json(const VideoBuildParameters& parameters);
// Throws std::invalid_argument naming the field for anything outside the
// schema above (unknown members included).
[[nodiscard]] VideoBuildParameters video_build_parameters_from_json(const nlohmann::json& value);
// nullopt when valid, otherwise why not (TaskParameterValidator).
[[nodiscard]] std::optional<std::string> validate_video_build_parameters(
    const nlohmann::json& value);

// A file name packages may record: non-empty, no '/', not "." or "..", no
// NUL, and within the file system's name limit.
[[nodiscard]] bool is_plain_file_name(std::string_view name) noexcept;

struct VideoBuildPackageRecord {
  std::string file_name;
  svp::exec::Blake3Digest blake3{};
  std::uint64_t bytes = 0;

  bool operator==(const VideoBuildPackageRecord&) const = default;
};

[[nodiscard]] std::string encode_video_build_package_record(const VideoBuildPackageRecord& record);
// Throws std::invalid_argument for anything but that record.
[[nodiscard]] VideoBuildPackageRecord decode_video_build_package_record(std::string_view bytes);

// The job's TaskSpec: `task_id` names the video in the batch.
struct VideoBuildSpecInput {
  std::string build_session_id;
  std::string task_id;
  VideoBuildParameters parameters;
  svp::exec::ArtifactRef source;
  svp::exec::ArtifactRef model_lock;
  std::vector<svp::exec::TaskModelRef> model_refs;
};
[[nodiscard]] svp::exec::TaskSpec make_video_build_spec(const VideoBuildSpecInput& input);

}  // namespace svp::builder::batch
