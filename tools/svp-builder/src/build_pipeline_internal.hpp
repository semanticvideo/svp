#pragma once

#include "svp/builder/build_pipeline.hpp"
#include "svp/media/media_ingest_plan.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>

namespace svp::builder {

struct BuildPipelineContext {
  const BuildPipelineOptions& options;
  const BuildStageExecutionPlan& stage_plan;
  const svp::media::MediaIngestPlan& plan;
  const std::filesystem::path& staging_dir;
  bool model_runtime_available = false;
  nlohmann::json& output;
};

struct PackageSkeletonStageResult {
  bool package_written = false;
  bool validator_passes = false;
  bool validation_report_stored = false;
  int validator_exit_code = -1;
  nlohmann::json validation_report_json = nlohmann::json::object();
  std::filesystem::path package_path;
  std::filesystem::path json_output_path;
};

void write_json_file(const std::filesystem::path& output_path,
                     const nlohmann::json& value);
void write_jsonl_file(const std::filesystem::path& output_path,
                      const nlohmann::json& records);
void append_jsonl_file(const std::filesystem::path& output_path,
                       const nlohmann::json& records);
svp::media::MediaProbe load_or_run_probe(const std::string& source_path,
                                         const std::string& probe_json_path,
                                         const std::string& ffprobe_path);
bool executable_exists(const std::filesystem::path& executable_path);

std::optional<int> run_audio_stage(BuildPipelineContext& context);
void run_vision_plan_stage(BuildPipelineContext& context);
void run_foundation_color_stage(BuildPipelineContext& context);
void run_foundation_ocr_stage(BuildPipelineContext& context);
PackageSkeletonStageResult run_package_skeleton_stage(BuildPipelineContext& context);
void print_build_progress(const BuildPipelineContext& context,
                          const PackageSkeletonStageResult& package_result);

}  // namespace svp::builder
