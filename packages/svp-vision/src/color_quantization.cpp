#include "svp/vision/color_quantization.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace svp::vision {
namespace {

constexpr double kPercentageSumTolerance = 0.001;

double srgb_channel_to_linear(const std::uint8_t channel) {
  const double value = static_cast<double>(channel) / 255.0;
  if (value <= 0.04045) {
    return value / 12.92;
  }
  return std::pow((value + 0.055) / 1.055, 2.4);
}

double cube_root(const double value) {
  return std::cbrt(value);
}

double normalize_degrees(double degrees) {
  degrees = std::fmod(degrees, 360.0);
  if (degrees < 0.0) {
    degrees += 360.0;
  }
  return degrees;
}

bool hue_in_range(const double hue_degrees,
                  const double inclusive_start,
                  const double exclusive_end) {
  return hue_degrees >= inclusive_start && hue_degrees < exclusive_end;
}

bool is_registered_target_type(const std::string& target_type) {
  static const std::array<std::string, 6> target_types = {
      "scene", "shot", "frame", "region", "entity", "text_region"};
  return std::find(target_types.begin(), target_types.end(), target_type) !=
         target_types.end();
}

bool is_registered_sampling_basis(const std::string& sampling_basis) {
  static const std::array<std::string, 8> sampling_basis_values = {
      "full_frame",
      "keyframe_full_frame",
      "mask",
      "region_crop",
      "entity_mask",
      "text_box",
      "text_foreground",
      "text_background",
  };
  return std::find(sampling_basis_values.begin(),
                   sampling_basis_values.end(),
                   sampling_basis) != sampling_basis_values.end();
}

}  // namespace

const std::vector<std::string>& registered_color_bucket_ids() {
  static const std::vector<std::string> bucket_ids = {
      "black", "white", "gray", "red",   "orange", "yellow", "green",
      "cyan",  "blue",  "purple", "pink", "brown",  "other"};
  return bucket_ids;
}

const std::string& registered_color_bucket_version() {
  static const std::string version = "svp-color-buckets-v1";
  return version;
}

const std::string& registered_color_space() {
  static const std::string color_space = "svp_oklch_v1";
  return color_space;
}

double registered_color_percentage_sum_tolerance() {
  return kPercentageSumTolerance;
}

OklchColor srgb8_to_oklch(const Srgb8Pixel pixel) {
  const double r = srgb_channel_to_linear(pixel.r);
  const double g = srgb_channel_to_linear(pixel.g);
  const double b = srgb_channel_to_linear(pixel.b);

  const double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
  const double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
  const double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;

  const double l_root = cube_root(l);
  const double m_root = cube_root(m);
  const double s_root = cube_root(s);

  const double oklab_l =
      0.2104542553 * l_root + 0.7936177850 * m_root - 0.0040720468 * s_root;
  const double oklab_a =
      1.9779984951 * l_root - 2.4285922050 * m_root + 0.4505937099 * s_root;
  const double oklab_b =
      0.0259040371 * l_root + 0.7827717662 * m_root - 0.8086757660 * s_root;

  const double chroma = std::hypot(oklab_a, oklab_b);
  const double hue_degrees = normalize_degrees(std::atan2(oklab_b, oklab_a) *
                                               180.0 / std::acos(-1.0));
  return OklchColor{oklab_l, chroma, hue_degrees};
}

std::string assign_registered_color_bucket(const OklchColor color) {
  if (color.l < 0.12) {
    return "black";
  }
  if (color.l > 0.92 && color.c < 0.08) {
    return "white";
  }
  if (color.c < 0.05) {
    return "gray";
  }
  if (color.c >= 0.05 && hue_in_range(color.h_degrees, 20.0, 75.0) &&
      color.l >= 0.12 && color.l <= 0.60) {
    return "brown";
  }
  if (color.c >= 0.05 &&
      (color.h_degrees < 25.0 || color.h_degrees >= 345.0)) {
    return "red";
  }
  if (color.c >= 0.05 && hue_in_range(color.h_degrees, 25.0, 55.0)) {
    return "orange";
  }
  if (color.c >= 0.05 && hue_in_range(color.h_degrees, 55.0, 85.0)) {
    return "yellow";
  }
  if (color.c >= 0.05 && hue_in_range(color.h_degrees, 85.0, 165.0)) {
    return "green";
  }
  if (color.c >= 0.05 && hue_in_range(color.h_degrees, 165.0, 215.0)) {
    return "cyan";
  }
  if (color.c >= 0.05 && hue_in_range(color.h_degrees, 215.0, 270.0)) {
    return "blue";
  }
  if (color.c >= 0.05 && hue_in_range(color.h_degrees, 270.0, 310.0)) {
    return "purple";
  }
  if (color.c >= 0.05 && hue_in_range(color.h_degrees, 310.0, 345.0)) {
    return "pink";
  }
  return "other";
}

QuantizedColorObservation summarize_color_samples(
    const ColorObservationTarget& target,
    const std::vector<Srgb8Pixel>& pixels) {
  if (!is_registered_target_type(target.target_type)) {
    throw std::invalid_argument("unregistered color observation target type: " +
                                target.target_type);
  }
  if (!is_registered_sampling_basis(target.sampling_basis)) {
    throw std::invalid_argument("unregistered color sampling basis: " +
                                target.sampling_basis);
  }

  QuantizedColorObservation observation;
  observation.target = target;
  observation.color_space = registered_color_space();
  observation.color_bucket_registry_version = registered_color_bucket_version();
  observation.quantization_method = "deterministic_srgb8_to_oklch_bucket_rules";
  observation.sampled_pixel_count = pixels.size();

  for (const std::string& bucket_id : registered_color_bucket_ids()) {
    observation.bucket_coverage[bucket_id] = 0.0;
  }

  if (pixels.empty()) {
    observation.dominant_bucket = "other";
    observation.quality_score = 0.0;
    return observation;
  }

  for (const Srgb8Pixel pixel : pixels) {
    const std::string bucket_id = assign_registered_color_bucket(srgb8_to_oklch(pixel));
    observation.bucket_coverage.at(bucket_id) += 1.0;
  }

  const double sampled_pixel_count = static_cast<double>(pixels.size());
  for (auto& [bucket_id, coverage] : observation.bucket_coverage) {
    coverage /= sampled_pixel_count;
  }

  observation.coverage_total =
      std::accumulate(observation.bucket_coverage.begin(),
                      observation.bucket_coverage.end(),
                      0.0,
                      [](const double total, const auto& bucket) {
                        return total + bucket.second;
                      });
  observation.dominant_bucket =
      std::max_element(observation.bucket_coverage.begin(),
                       observation.bucket_coverage.end(),
                       [](const auto& lhs, const auto& rhs) {
                         return lhs.second < rhs.second;
                       })
          ->first;
  observation.quality_score = 1.0;
  return observation;
}

nlohmann::json quantized_color_observation_to_json(
    const QuantizedColorObservation& observation) {
  return {
      {"target_type", observation.target.target_type},
      {"target_id", observation.target.target_id},
      {"start_us", observation.target.start_us},
      {"end_us", observation.target.end_us},
      {"frame_ids", observation.target.frame_ids},
      {"sampling_basis", observation.target.sampling_basis},
      {"color_space", observation.color_space},
      {"color_bucket_registry_version",
       observation.color_bucket_registry_version},
      {"bucket_coverage", observation.bucket_coverage},
      {"dominant_bucket", observation.dominant_bucket},
      {"coverage_total", observation.coverage_total},
      {"quality_score", observation.quality_score},
      {"sampled_pixel_count", observation.sampled_pixel_count},
      {"quantization_method", observation.quantization_method},
  };
}

}  // namespace svp::vision
