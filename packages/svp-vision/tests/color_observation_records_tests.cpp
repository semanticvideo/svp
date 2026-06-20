#include "svp/vision/color_observation_records.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    std::exit(1);
  }
}

void require_near(const double actual,
                  const double expected,
                  const double tolerance,
                  const std::string& message) {
  if (std::fabs(actual - expected) > tolerance) {
    std::cerr << message << ": expected " << expected << ", got " << actual << "\n";
    std::exit(1);
  }
}

svp::vision::QuantizedColorObservation summary_for(
    svp::vision::ColorObservationTarget target,
    std::vector<svp::vision::Srgb8Pixel> pixels) {
  return svp::vision::summarize_color_samples(target, pixels);
}

svp::vision::ColorObservationTargetReferences target_references() {
  return svp::vision::ColorObservationTargetReferences{
      {{"scene", {"scene_000001"}},
       {"shot", {"shot_000001"}},
       {"frame", {"frame_000001"}},
       {"region", {"region_000001"}},
       {"text_region", {"text_region_000001"}}},
  };
}

svp::vision::ColorObservationConstructionOptions options() {
  return svp::vision::ColorObservationConstructionOptions{
      "processor_color_quantizer_0001",
      "color_obs_",
      1,
  };
}

svp::vision::ColorObservationTarget target(const std::string& target_type,
                                           const std::string& target_id,
                                           const std::string& sampling_basis) {
  return svp::vision::ColorObservationTarget{
      target_type,
      target_id,
      0,
      1000000,
      {"frame_000001"},
      sampling_basis,
  };
}

}  // namespace

int main() {
  const std::vector<svp::vision::QuantizedColorObservation> summaries = {
      summary_for(target("scene", "scene_000001", "full_frame"),
                  {{255, 128, 0}, {255, 128, 0}, {255, 255, 255}, {0, 0, 0}}),
      summary_for(target("shot", "shot_000001", "keyframe_full_frame"),
                  {{255, 160, 0}, {255, 160, 0}, {255, 128, 0}, {0, 0, 0}}),
      summary_for(target("frame", "frame_000001", "full_frame"),
                  {{0, 0, 0}, {0, 0, 0}, {255, 255, 255}, {255, 128, 0}}),
      summary_for(target("region", "region_000001", "region_crop"),
                  {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {0, 0, 0}}),
      summary_for(target("text_region", "text_region_000001", "text_box"),
                  {{255, 255, 255}, {255, 255, 255}, {0, 0, 0}, {0, 0, 0}}),
  };

  const svp::vision::ColorObservationRecordPlan plan =
      svp::vision::plan_color_observation_records(
          summaries, target_references(), options());

  require(plan.records.size() == summaries.size(),
          "record plan should preserve one record per quantized summary");
  require(plan.records.front().color_observation_id == "color_obs_000001",
          "record ids should use canonical six-digit color observation ids");
  require(plan.records.back().color_observation_id == "color_obs_000005",
          "record ids should increment in input order");
  require(plan.records.at(1).target.target_type == "shot",
          "target type should be copied from the quantized summary");
  require(plan.records.at(1).target.target_id == "shot_000001",
          "target id should be copied from the quantized summary");
  require(plan.records.at(3).dominant_bucket == "orange",
          "dominant bucket should be preserved from quantized coverage");
  require_near(plan.records.at(3).coverage_total, 1.0, 0.0000001,
               "record coverage totals should remain normalized");

  const nlohmann::json record_json =
      svp::vision::color_observation_record_to_json(plan.records.front());
  require(record_json.at("color_observation_id") == "color_obs_000001",
          "record JSON should include color observation id");
  require(record_json.at("target_type") == "scene",
          "record JSON should include target type");
  require(record_json.at("provenance_id") == "processor_color_quantizer_0001",
          "record JSON should include provenance id");
  require(record_json.at("bucket_coverage").at("orange") == 0.5,
          "record JSON should include bucket coverage percentages");
  require(!record_json.contains("sampled_pixel_count"),
          "schema-shaped record JSON must not include quantizer-only sample counts");
  require(!record_json.contains("quantization_method"),
          "schema-shaped record JSON must not include quantizer-only method fields");

  const nlohmann::json records_json =
      svp::vision::color_observation_records_to_jsonl_array(plan.records);
  require(records_json.is_array() && records_json.size() == summaries.size(),
          "records should serialize as one JSON object per JSONL row");

  require(plan.color_summary.at("schema_version") == "svp-color-summary-v1",
          "summary JSON should use the RC2 color summary schema version");
  require(plan.color_summary.at("color_observation_count") == summaries.size(),
          "summary JSON should count planned observations");
  require(plan.color_absence.at("schema_version") == "svp-color-absence-v1",
          "absence JSON should use the RC2 color absence schema version");
  require(plan.color_absence.at("color_completed") == true,
          "absence JSON should honestly report completed construction from summaries");
  require(plan.color_absence.at("reason") == "color_observed",
          "absence JSON should report observed color when records exist");

  std::vector<svp::vision::QuantizedColorObservation> invalid_target = summaries;
  invalid_target.front().target.target_id = "scene_missing";
  bool rejected_missing_target = false;
  try {
    (void)svp::vision::plan_color_observation_records(
        invalid_target, target_references(), options());
  } catch (const std::invalid_argument&) {
    rejected_missing_target = true;
  }
  require(rejected_missing_target,
          "record construction should reject missing target references");

  std::vector<svp::vision::QuantizedColorObservation> invalid_total = summaries;
  invalid_total.front().bucket_coverage.at("orange") = 0.25;
  bool rejected_invalid_total = false;
  try {
    (void)svp::vision::plan_color_observation_records(
        invalid_total, target_references(), options());
  } catch (const std::invalid_argument&) {
    rejected_invalid_total = true;
  }
  require(rejected_invalid_total,
          "record construction should reject bucket percentages that do not total one");

  std::vector<svp::vision::QuantizedColorObservation> invalid_dominant = summaries;
  invalid_dominant.front().dominant_bucket = "black";
  bool rejected_invalid_dominant = false;
  try {
    (void)svp::vision::plan_color_observation_records(
        invalid_dominant, target_references(), options());
  } catch (const std::invalid_argument&) {
    rejected_invalid_dominant = true;
  }
  require(rejected_invalid_dominant,
          "record construction should reject a non-dominant bucket claim");

  return 0;
}
