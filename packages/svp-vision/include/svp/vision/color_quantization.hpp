#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace svp::vision {

struct Srgb8Pixel {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
};

struct OklchColor {
  double l;
  double c;
  double h_degrees;
};

struct ColorObservationTarget {
  std::string target_type;
  std::string target_id;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::vector<std::string> frame_ids;
  std::string sampling_basis;
};

struct QuantizedColorObservation {
  ColorObservationTarget target;
  std::string color_space;
  std::string color_bucket_registry_version;
  std::map<std::string, double> bucket_coverage;
  std::string dominant_bucket;
  double coverage_total = 0.0;
  double quality_score = 0.0;
  std::size_t sampled_pixel_count = 0;
  std::string quantization_method;
};

[[nodiscard]] const std::vector<std::string>& registered_color_bucket_ids();
[[nodiscard]] const std::string& registered_color_bucket_version();
[[nodiscard]] const std::string& registered_color_space();
[[nodiscard]] double registered_color_percentage_sum_tolerance();

[[nodiscard]] OklchColor srgb8_to_oklch(Srgb8Pixel pixel);
[[nodiscard]] std::string assign_registered_color_bucket(OklchColor color);
[[nodiscard]] QuantizedColorObservation summarize_color_samples(
    const ColorObservationTarget& target,
    const std::vector<Srgb8Pixel>& pixels);
[[nodiscard]] nlohmann::json quantized_color_observation_to_json(
    const QuantizedColorObservation& observation);

}  // namespace svp::vision
