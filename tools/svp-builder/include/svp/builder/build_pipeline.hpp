#pragma once

#include <filesystem>
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
  BuildStage stop_after = BuildStage::media_ingest;
  std::string sherpa_lib_path;
  bool allow_fallback_diarization = false;
  bool force_single_speaker = false;
};

struct BuildPipelineResult {
  int exit_code = 0;
};

std::vector<std::string_view> supported_build_stage_names();
std::optional<BuildStage> parse_build_stage(std::string_view value);
std::string_view build_stage_name(BuildStage stage);
BuildStageExecutionPlan execution_plan_for_stage(BuildStage stage);
std::filesystem::path default_staging_dir_for_output(
    const std::filesystem::path& output_path);
BuildOutputPaths resolve_package_skeleton_output_paths(
    const std::filesystem::path& output_path);

class BuildPipeline {
 public:
  BuildPipelineResult run(const BuildPipelineOptions& options) const;
};

}  // namespace svp::builder
