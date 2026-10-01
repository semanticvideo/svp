#pragma once

// What every whole-stage task function sees: the build's plan-time inputs
// (options, ingest plan, thread plan, the planned frame catalog, staging
// directory) and the committed results of the tasks it depends on. Nothing
// else is shared between tasks: each task gets its own foundation-JSON object
// and its own copy of the planned frame catalog (StageTaskContext), and
// returns everything later tasks need as named states.

#include "build_pipeline_internal.hpp"
#include "engine/committed_stage_results.hpp"

#include "svp/builder/build_pipeline.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/tasks/pp_ocr_session_pool.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace svp::builder::engine {

struct OcrFrameBatchPlan;

// A stage that ends the build with a specific exit status rather than an
// error (the audio lane when diarization is required but unavailable).
class StageExitError : public std::runtime_error {
 public:
  explicit StageExitError(int exit_code)
      : std::runtime_error("stage requested exit status " + std::to_string(exit_code)),
        exit_code_(exit_code) {}
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

 private:
  int exit_code_;
};

struct StageTaskEnvironment {
  const BuildPipelineOptions& options;
  const BuildStageExecutionPlan& stage_plan;
  const svp::media::MediaIngestPlan& plan;
  // media_ingest_plan_to_json(plan): the builder foundation JSON's base.
  const nlohmann::json& plan_json;
  const std::filesystem::path& staging_dir;
  const svp::models::ThreadPlan& thread_plan;
  bool model_runtime_available = false;
  // Locked frame plan (#159); tasks copy it, never mutate it.
  const svp::vision::FrameCatalog& planned_catalog;
  BuildProgressSink& progress_sink;
  const CommittedStageResults& results;
  // Set when the OCR stage runs as frame-batch tasks: the `ocr` task reduces
  // their committed outputs instead of running the whole stage.
  const OcrFrameBatchPlan* ocr_batches = nullptr;
  // PP-OCR sessions shared by this process's frame-batch tasks and the OCR
  // reducer's evidence-crop re-read.
  std::shared_ptr<svp::vision::tasks::PpOcrSessionPool> pp_ocr_sessions;
};

// Named state blobs a task returns (stage_task_products.hpp).
using StageStates = std::map<std::string, std::vector<std::byte>>;

// State names shared by producers and consumers.
namespace state_name {
// JSON object of builder foundation keys the task sets.
inline constexpr const char* kFoundation = "foundation";
// frame_catalog_delta of the task's catalog copy.
inline constexpr const char* kFrameCatalog = "frame_catalog";
}  // namespace state_name

// A BuildPipelineContext for one task, over its own foundation-JSON object and
// its own copy of the planned frame catalog.
class StageTaskContext {
 public:
  explicit StageTaskContext(const StageTaskEnvironment& environment)
      : context_{environment.options,      environment.stage_plan,
                 environment.plan,         environment.staging_dir,
                 environment.thread_plan,  environment.model_runtime_available,
                 output_,                  environment.planned_catalog,
                 environment.progress_sink} {}
  StageTaskContext(const StageTaskContext&) = delete;
  StageTaskContext& operator=(const StageTaskContext&) = delete;

  [[nodiscard]] BuildPipelineContext& context() { return context_; }
  [[nodiscard]] const nlohmann::json& output() const { return output_; }

 private:
  nlohmann::json output_ = nlohmann::json::object();
  BuildPipelineContext context_;
};

}  // namespace svp::builder::engine
