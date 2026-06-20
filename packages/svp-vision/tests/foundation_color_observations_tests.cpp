#include "svp/vision/foundation_color_observations.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
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

svp::vision::ColorRasterFrame frame(
    std::string frame_id,
    const std::int64_t timestamp_us,
    const bool keyframe,
    std::vector<svp::vision::Srgb8Pixel> pixels) {
  return svp::vision::ColorRasterFrame{
      std::move(frame_id),
      timestamp_us,
      2,
      2,
      keyframe,
      std::move(pixels),
  };
}

svp::vision::ColorTimelineRange range(std::string target_id,
                                      const std::int64_t start_us,
                                      const std::int64_t end_us,
                                      std::vector<std::string> frame_ids) {
  return svp::vision::ColorTimelineRange{
      std::move(target_id),
      start_us,
      end_us,
      std::move(frame_ids),
  };
}

svp::vision::ColorFrameSamplingInput sample_input() {
  return svp::vision::ColorFrameSamplingInput{
      {
          frame("frame_000002", 500000, false,
                {{255, 255, 255}, {255, 255, 255}, {0, 0, 0}, {0, 0, 0}}),
          frame("frame_000001", 0, true,
                {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {0, 0, 0}}),
          frame("frame_000003", 1000000, true,
                {{255, 160, 0}, {255, 160, 0}, {255, 160, 0}, {255, 160, 0}}),
      },
      {
          range("scene_000001", 0, 1000000,
                {"frame_000001", "frame_000002", "frame_000003"}),
      },
      {
          range("shot_000001", 0, 500000, {"frame_000001", "frame_000002"}),
          range("shot_000002", 500000, 1000000,
                {"frame_000002", "frame_000003"}),
      },
  };
}

const svp::vision::ColorObservationRecord& find_record(
    const std::vector<svp::vision::ColorObservationRecord>& records,
    const std::string& target_type,
    const std::string& target_id) {
  for (const svp::vision::ColorObservationRecord& record : records) {
    if (record.target.target_type == target_type &&
        record.target.target_id == target_id) {
      return record;
    }
  }
  std::cerr << "missing record for " << target_type << "/" << target_id << "\n";
  std::exit(1);
}

double bucket_total(const svp::vision::ColorObservationRecord& record) {
  double total = 0.0;
  for (const auto& [_, coverage] : record.bucket_coverage) {
    total += coverage;
  }
  return total;
}

}  // namespace

int main() {
  const svp::vision::ColorObservationRecordPlan plan =
      svp::vision::build_foundation_color_observations(
          sample_input(),
          svp::vision::FoundationColorObservationOptions{
              "processor_color_quantizer_0007",
              42,
          });

  require(plan.records.size() == 6,
          "foundation color observations should cover frames, scenes, and shots");
  require(plan.records.front().color_observation_id == "color_obs_000042",
          "record ids should start from the requested deterministic number");
  require(plan.records.back().color_observation_id == "color_obs_000047",
          "record ids should remain contiguous across sampled targets");
  require(plan.color_summary.at("color_observation_count") == 6,
          "color summary should count foundation records");
  require(plan.color_summary.at("provenance_id") == "processor_color_quantizer_0007",
          "color summary should carry color quantizer provenance");
  require(plan.color_absence.at("color_completed") == true,
          "color absence should report completed deterministic color processing");
  require(plan.color_absence.at("reason") == "color_observed",
          "color absence should report observed color when records exist");

  const svp::vision::ColorObservationRecord& frame_record =
      find_record(plan.records, "frame", "frame_000001");
  require(frame_record.target.sampling_basis == "full_frame",
          "frame records should use full-frame sampling");
  require(frame_record.target.frame_ids == std::vector<std::string>{"frame_000001"},
          "frame records should reference their sampled frame");
  require(frame_record.dominant_bucket == "orange",
          "frame record should preserve the dominant sampled bucket");
  require_near(frame_record.bucket_coverage.at("orange"), 0.75, 0.0000001,
               "frame orange coverage should reflect sampled pixels");
  require_near(frame_record.bucket_coverage.at("black"), 0.25, 0.0000001,
               "frame black coverage should reflect sampled pixels");
  require_near(bucket_total(frame_record), 1.0, 0.0000001,
               "frame bucket percentages should total one");
  require_near(frame_record.coverage_total, 1.0, 0.0000001,
               "frame coverage total should be normalized");

  const svp::vision::ColorObservationRecord& scene_record =
      find_record(plan.records, "scene", "scene_000001");
  require(scene_record.target.sampling_basis == "keyframe_full_frame",
          "scene records should advertise keyframe sampling");
  require(scene_record.target.frame_ids ==
              std::vector<std::string>({"frame_000001", "frame_000003"}),
          "scene records should include deterministic keyframes only");
  require(scene_record.dominant_bucket == "yellow",
          "scene record should preserve the dominant keyframe bucket");
  require_near(scene_record.bucket_coverage.at("orange"), 0.375, 0.0000001,
               "scene orange coverage should reflect sampled keyframes");
  require_near(scene_record.bucket_coverage.at("yellow"), 0.5, 0.0000001,
               "scene yellow coverage should reflect sampled keyframes");
  require_near(scene_record.bucket_coverage.at("black"), 0.125, 0.0000001,
               "scene black coverage should reflect sampled keyframes");
  require_near(bucket_total(scene_record), 1.0, 0.0000001,
               "scene bucket percentages should total one");

  const svp::vision::ColorObservationRecord& first_shot_record =
      find_record(plan.records, "shot", "shot_000001");
  require(first_shot_record.target.frame_ids ==
              std::vector<std::string>({"frame_000001"}),
          "shot records should prefer keyframes in their target range");
  require(first_shot_record.dominant_bucket == "orange",
          "shot record should use the sampled keyframe distribution");

  const svp::vision::ColorObservationRecord& second_shot_record =
      find_record(plan.records, "shot", "shot_000002");
  require(second_shot_record.target.frame_ids ==
              std::vector<std::string>({"frame_000003"}),
          "shot records should choose the keyframe available in the range");
  require(second_shot_record.dominant_bucket == "yellow",
          "shot record should preserve the dominant bucket from sampled pixels");
  require_near(second_shot_record.bucket_coverage.at("yellow"), 1.0, 0.0000001,
               "shot coverage should reflect all sampled keyframe pixels");

  const nlohmann::json record_json =
      svp::vision::color_observation_record_to_json(scene_record);
  require(record_json.at("color_space") == "svp_oklch_v1",
          "record JSON should use the registered color space");
  require(record_json.at("color_bucket_registry_version") ==
              "svp-color-buckets-v1",
          "record JSON should use the registered bucket version");
  require(record_json.at("provenance_id") == "processor_color_quantizer_0007",
          "record JSON should include provenance");

  return 0;
}
