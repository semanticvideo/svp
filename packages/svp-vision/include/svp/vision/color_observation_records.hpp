#pragma once

#include "svp/vision/color_quantization.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace svp::vision {

struct ColorObservationTargetReferences {
  std::map<std::string, std::vector<std::string>> ids_by_target_type;
};

struct ColorObservationConstructionOptions {
  std::string provenance_id;
  std::string observation_id_prefix = "color_obs_";
  int first_observation_number = 1;
};

struct ColorObservationRecord {
  std::string color_observation_id;
  ColorObservationTarget target;
  std::string color_space;
  std::string color_bucket_registry_version;
  std::map<std::string, double> bucket_coverage;
  std::string dominant_bucket;
  double coverage_total = 0.0;
  double quality_score = 0.0;
  std::string provenance_id;
};

struct ColorObservationRecordPlan {
  std::vector<ColorObservationRecord> records;
  nlohmann::json color_summary;
  nlohmann::json color_absence;
};

[[nodiscard]] ColorObservationRecordPlan plan_color_observation_records(
    const std::vector<QuantizedColorObservation>& summaries,
    const ColorObservationTargetReferences& target_references,
    const ColorObservationConstructionOptions& options);
[[nodiscard]] nlohmann::json color_observation_record_to_json(
    const ColorObservationRecord& record);
[[nodiscard]] nlohmann::json color_observation_records_to_jsonl_array(
    const std::vector<ColorObservationRecord>& records);

}  // namespace svp::vision
