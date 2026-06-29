#pragma once

#include "svp/vision/color_observation_records.hpp"
#include "svp/vision/color_quantization.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace svp::vision {

struct ColorRasterFrame {
  std::string frame_id;
  std::int64_t timestamp_us = 0;
  int width = 0;
  int height = 0;
  bool keyframe = false;
  std::vector<Srgb8Pixel> pixels;
  std::size_t frame_index = 0;
};

struct ColorTimelineRange {
  std::string target_id;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::vector<std::string> frame_ids;
};

struct ColorFrameSamplingInput {
  std::vector<ColorRasterFrame> frames;
  std::vector<ColorTimelineRange> scenes;
  std::vector<ColorTimelineRange> shots;
};

struct SampledColorObservationPlan {
  std::vector<QuantizedColorObservation> summaries;
  ColorObservationTargetReferences target_references;
};

[[nodiscard]] SampledColorObservationPlan sample_frame_scene_shot_colors(
    const ColorFrameSamplingInput& input);

}  // namespace svp::vision
