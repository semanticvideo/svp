#pragma once

#include "svp/media/media_ingest_plan.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace svp::vision {

enum class ObservationTaskFamily {
  ocr,
  color,
  provenance,
};

struct ObservationPipelineTask {
  std::string task_id;
  ObservationTaskFamily family;
  std::string processor_boundary;
  std::vector<std::string> depends_on;
  std::vector<std::string> consumes;
  std::vector<std::string> emits;
  std::string provenance_hook;
  bool model_backed;
  bool deterministic_classical;
};

struct VisionObservationPipelinePlan {
  std::string schema_version;
  std::string source_path;
  std::string canonical_raster_basis;
  std::string color_space;
  std::string color_bucket_registry_version;
  double color_percentage_sum_tolerance;
  std::vector<ObservationPipelineTask> tasks;
  std::vector<std::string> explicit_non_goals;
};

[[nodiscard]] VisionObservationPipelinePlan build_vision_observation_pipeline_plan(
    const media::MediaIngestPlan& media_plan);
[[nodiscard]] nlohmann::json vision_observation_pipeline_plan_to_json(
    const VisionObservationPipelinePlan& plan);

}  // namespace svp::vision
