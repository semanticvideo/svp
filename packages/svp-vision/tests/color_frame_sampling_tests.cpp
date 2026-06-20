#include "svp/vision/color_frame_sampling.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
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

const svp::vision::QuantizedColorObservation& find_summary(
    const std::vector<svp::vision::QuantizedColorObservation>& summaries,
    const std::string& target_type,
    const std::string& target_id) {
  for (const svp::vision::QuantizedColorObservation& summary : summaries) {
    if (summary.target.target_type == target_type &&
        summary.target.target_id == target_id) {
      return summary;
    }
  }
  std::cerr << "missing summary for " << target_type << "/" << target_id << "\n";
  std::exit(1);
}

svp::vision::ColorFrameSamplingInput sample_input() {
  return svp::vision::ColorFrameSamplingInput{
      {
          frame("frame_000001", 0, true,
                {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {0, 0, 0}}),
          frame("frame_000002", 500000, false,
                {{255, 255, 255}, {255, 255, 255}, {0, 0, 0}, {0, 0, 0}}),
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

}  // namespace

int main() {
  const svp::vision::SampledColorObservationPlan sampled =
      svp::vision::sample_frame_scene_shot_colors(sample_input());

  require(sampled.summaries.size() == 6,
          "sampling should emit one summary for each frame, scene, and shot");
  require(sampled.target_references.ids_by_target_type.at("frame").size() == 3,
          "target references should include sampled frames");
  require(sampled.target_references.ids_by_target_type.at("scene").size() == 1,
          "target references should include sampled scenes");
  require(sampled.target_references.ids_by_target_type.at("shot").size() == 2,
          "target references should include sampled shots");

  const svp::vision::QuantizedColorObservation& frame_summary =
      find_summary(sampled.summaries, "frame", "frame_000001");
  require(frame_summary.target.frame_ids == std::vector<std::string>{"frame_000001"},
          "frame summaries should reference their source frame");
  require(frame_summary.target.sampling_basis == "full_frame",
          "frame summaries should use full-frame sampling");
  require(frame_summary.dominant_bucket == "orange",
          "frame summary should use real sampled pixels");
  require_near(frame_summary.bucket_coverage.at("orange"), 0.75, 0.0000001,
               "frame orange coverage should reflect all frame pixels");
  require_near(frame_summary.bucket_coverage.at("black"), 0.25, 0.0000001,
               "frame black coverage should reflect all frame pixels");

  const svp::vision::QuantizedColorObservation& scene_summary =
      find_summary(sampled.summaries, "scene", "scene_000001");
  require(scene_summary.target.frame_ids ==
              std::vector<std::string>({"frame_000001", "frame_000003"}),
          "scene summaries should sample deterministic keyframes");
  require(scene_summary.target.sampling_basis == "keyframe_full_frame",
          "scene summaries should advertise keyframe sampling");
  require(scene_summary.dominant_bucket == "yellow",
          "scene summary should aggregate keyframe pixels deterministically");
  require_near(scene_summary.bucket_coverage.at("orange"), 0.375, 0.0000001,
               "scene orange coverage should include first keyframe pixels");
  require_near(scene_summary.bucket_coverage.at("yellow"), 0.5, 0.0000001,
               "scene yellow coverage should include second keyframe pixels");
  require_near(scene_summary.bucket_coverage.at("black"), 0.125, 0.0000001,
               "scene black coverage should include first keyframe pixels");

  const svp::vision::QuantizedColorObservation& first_shot_summary =
      find_summary(sampled.summaries, "shot", "shot_000001");
  require(first_shot_summary.target.frame_ids ==
              std::vector<std::string>({"frame_000001"}),
          "shot summaries should prefer keyframes when the range has one");

  svp::vision::ColorFrameSamplingInput fallback_input = sample_input();
  fallback_input.shots = {
      range("shot_000003", 0, 500000, {"frame_000002"}),
  };
  const svp::vision::SampledColorObservationPlan fallback_sampled =
      svp::vision::sample_frame_scene_shot_colors(fallback_input);
  const svp::vision::QuantizedColorObservation& fallback_shot_summary =
      find_summary(fallback_sampled.summaries, "shot", "shot_000003");
  require(fallback_shot_summary.target.frame_ids ==
              std::vector<std::string>({"frame_000002"}),
          "shot summaries should fall back to referenced frames when no keyframe exists");
  require_near(fallback_shot_summary.bucket_coverage.at("white"), 0.5, 0.0000001,
               "fallback shot summary should still use real frame pixels");
  require_near(fallback_shot_summary.bucket_coverage.at("black"), 0.5, 0.0000001,
               "fallback shot summary should still use real frame pixels");

  const svp::vision::ColorObservationRecordPlan records =
      svp::vision::plan_color_observation_records(
          sampled.summaries,
          sampled.target_references,
          svp::vision::ColorObservationConstructionOptions{
              "processor_color_quantizer_0001", "color_obs_", 1});
  require(records.records.size() == sampled.summaries.size(),
          "sampled summaries should feed the color observation record planner");
  require(records.color_absence.at("color_completed") == true,
          "record planning from sampled summaries should report completed color processing");

  svp::vision::ColorFrameSamplingInput invalid_pixels = sample_input();
  invalid_pixels.frames.front().pixels.pop_back();
  bool rejected_bad_pixels = false;
  try {
    (void)svp::vision::sample_frame_scene_shot_colors(invalid_pixels);
  } catch (const std::invalid_argument&) {
    rejected_bad_pixels = true;
  }
  require(rejected_bad_pixels,
          "sampling should reject frames whose pixels do not match dimensions");

  svp::vision::ColorFrameSamplingInput missing_frame = sample_input();
  missing_frame.scenes.front().frame_ids.push_back("frame_missing");
  bool rejected_missing_frame = false;
  try {
    (void)svp::vision::sample_frame_scene_shot_colors(missing_frame);
  } catch (const std::invalid_argument&) {
    rejected_missing_frame = true;
  }
  require(rejected_missing_frame,
          "sampling should reject ranges that reference missing frames");

  return 0;
}
