#pragma once

#include "svp/vision/color_frame_sampling.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace svp::vision {

struct VisualEntityCutDetectionOptions {
  double immediate_difference_threshold = 0.08;
  double stable_difference_threshold = 0.01;
  // A change this large is a hard full-frame replacement even when the new
  // scene itself contains motion and therefore does not immediately settle.
  double hard_cut_difference_threshold = 0.30;
  // Dissolves and animated full-frame transitions do not settle after one
  // observation. Three consecutive mean changes above 0.12 indicate a
  // sustained scene replacement rather than a single moving foreground item.
  double sustained_transition_difference_threshold = 0.12;
  std::size_t sustained_transition_observations = 3;
  std::size_t extended_stability_lookahead_frames = 3;
};

struct VisualEntityCutEvidence {
  std::int64_t timestamp_us = 0;
  double difference = 0.0;
  double immediate_following_difference = 1.0;
  double minimum_lookahead_difference = 1.0;
  bool is_sustained_transition = false;
  bool is_cut = false;
};

[[nodiscard]] std::vector<VisualEntityCutEvidence>
detect_visual_entity_cuts(
    const std::vector<ColorRasterFrame>& frames,
    const VisualEntityCutDetectionOptions& options = {});

}  // namespace svp::vision
