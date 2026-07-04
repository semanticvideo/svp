#pragma once

#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_progress.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/frame_catalog.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace svp::builder {

struct BuildPipelineContext {
  const BuildPipelineOptions& options;
  const BuildStageExecutionPlan& stage_plan;
  const svp::media::MediaIngestPlan& plan;
  const std::filesystem::path& staging_dir;
  bool model_runtime_available = false;
  nlohmann::json& output;
  svp::vision::FrameCatalog frame_catalog;
  BuildProgressSink& progress_sink;
};

void emit_progress(BuildPipelineContext& context, const ProgressEvent& event);
void emit_stage_started(BuildPipelineContext& context, ProgressStageId stage,
                        std::string message = "");
void emit_stage_completed(BuildPipelineContext& context, ProgressStageId stage,
                          std::string message = "");
void emit_stage_failed(BuildPipelineContext& context, ProgressStageId stage,
                       std::string message = "");
void emit_warning(BuildPipelineContext& context, ProgressStageId stage,
                  std::string message);
void emit_artifact_written(BuildPipelineContext& context, ProgressStageId stage,
                           std::filesystem::path artifact_path,
                           std::string message = "");
void emit_stage_progress(BuildPipelineContext& context, ProgressStageId stage,
                         std::uint64_t current, std::uint64_t total,
                         std::string unit, std::string message = "");

struct PackageSkeletonStageResult {
  bool package_written = false;
  bool validator_passes = false;
  bool validation_report_stored = false;
  int validator_exit_code = -1;
  nlohmann::json validation_report_json = nlohmann::json::object();
  std::filesystem::path package_path;
  std::filesystem::path json_output_path;
};

struct PackageVisionStageResult {
  nlohmann::json placeholder_summary_json = nlohmann::json::object();
  std::vector<nlohmann::json> processor_records;
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
PackageVisionStageResult run_package_vision_stage(BuildPipelineContext& context);
PackageSkeletonStageResult run_package_final_stage(
    BuildPipelineContext& context,
    const PackageVisionStageResult& vision_result);
PackageSkeletonStageResult run_package_skeleton_stage(BuildPipelineContext& context);
void print_build_progress(const BuildPipelineContext& context,
                          const PackageSkeletonStageResult& package_result);

}  // namespace svp::builder
