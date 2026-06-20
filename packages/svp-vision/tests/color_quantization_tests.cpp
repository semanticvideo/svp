#include "svp/vision/color_quantization.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
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

svp::vision::ColorObservationTarget sample_target() {
  return svp::vision::ColorObservationTarget{
      "shot",
      "shot_000001",
      0,
      1000000,
      {"frame_000001"},
      "keyframe_full_frame",
  };
}

}  // namespace

int main() {
  require(svp::vision::registered_color_bucket_version() ==
              "svp-color-buckets-v1",
          "color bucket registry version must match RC2 registry");
  require(svp::vision::registered_color_space() == "svp_oklch_v1",
          "color space must match RC2 bucket registry canonical space");

  require(svp::vision::assign_registered_color_bucket(
              svp::vision::srgb8_to_oklch({0, 0, 0})) == "black",
          "black pixel should assign to black bucket");
  require(svp::vision::assign_registered_color_bucket(
              svp::vision::srgb8_to_oklch({255, 255, 255})) == "white",
          "white pixel should assign to white bucket");
  require(svp::vision::assign_registered_color_bucket(
              svp::vision::srgb8_to_oklch({255, 128, 0})) == "orange",
          "orange pixel should assign to orange bucket");
  require(svp::vision::assign_registered_color_bucket(
              svp::vision::srgb8_to_oklch({255, 160, 0})) == "yellow",
          "yellow pixel should assign to yellow bucket");

  const std::vector<svp::vision::Srgb8Pixel> pixels = {
      {255, 128, 0},
      {255, 128, 0},
      {255, 160, 0},
      {255, 255, 255},
  };
  const svp::vision::QuantizedColorObservation observation =
      svp::vision::summarize_color_samples(sample_target(), pixels);

  require(observation.sampled_pixel_count == pixels.size(),
          "sampled pixel count should be recorded");
  require(observation.dominant_bucket == "orange",
          "orange should be the dominant bucket");
  require_near(observation.bucket_coverage.at("orange"), 0.5, 0.0000001,
               "orange coverage should reflect sample pixels");
  require_near(observation.bucket_coverage.at("yellow"), 0.25, 0.0000001,
               "yellow coverage should reflect sample pixels");
  require_near(observation.bucket_coverage.at("white"), 0.25, 0.0000001,
               "white coverage should reflect sample pixels");
  require_near(observation.coverage_total, 1.0, 0.0000001,
               "coverage total should sum to one");
  require(std::fabs(observation.coverage_total - 1.0) <=
              svp::vision::registered_color_percentage_sum_tolerance(),
          "coverage total should satisfy registered tolerance");

  const nlohmann::json json =
      svp::vision::quantized_color_observation_to_json(observation);
  require(json.at("color_bucket_registry_version") ==
              svp::vision::registered_color_bucket_version(),
          "JSON should include registry version");
  require(json.at("bucket_coverage").at("orange") == 0.5,
          "JSON should include bucket coverage");

  return 0;
}
