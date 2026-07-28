#include "svp/vision/visual_entity_cut_detection.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace svp::vision {
namespace {

double normalized_mean_difference(
    const ColorRasterFrame& left,
    const ColorRasterFrame& right) {
  if (left.pixels.empty() || left.pixels.size() != right.pixels.size()) {
    return 1.0;
  }
  double difference_sum = 0.0;
  for (std::size_t index = 0; index < left.pixels.size(); ++index) {
    difference_sum += std::abs(
        static_cast<int>(left.pixels[index].r) - right.pixels[index].r);
    difference_sum += std::abs(
        static_cast<int>(left.pixels[index].g) - right.pixels[index].g);
    difference_sum += std::abs(
        static_cast<int>(left.pixels[index].b) - right.pixels[index].b);
  }
  return difference_sum /
      (static_cast<double>(left.pixels.size()) * 3.0 * 255.0);
}

}  // namespace

std::vector<VisualEntityCutEvidence> detect_visual_entity_cuts(
    const std::vector<ColorRasterFrame>& frames,
    const VisualEntityCutDetectionOptions& options) {
  if (options.immediate_difference_threshold <= 0.0 ||
      options.immediate_difference_threshold > 1.0 ||
      options.stable_difference_threshold <= 0.0 ||
      options.stable_difference_threshold > 1.0 ||
      options.hard_cut_difference_threshold <
          options.immediate_difference_threshold ||
      options.hard_cut_difference_threshold > 1.0 ||
      options.sustained_transition_difference_threshold <= 0.0 ||
      options.sustained_transition_difference_threshold > 1.0 ||
      options.sustained_transition_observations < 2 ||
      options.extended_stability_lookahead_frames == 0) {
    throw std::invalid_argument("invalid visual entity cut detection policy");
  }

  std::vector<VisualEntityCutEvidence> evidence;
  if (frames.size() < 2) return evidence;
  evidence.reserve(frames.size() - 1);
  bool sustained_transition_in_progress = false;
  for (std::size_t index = 1; index < frames.size(); ++index) {
    VisualEntityCutEvidence item;
    item.timestamp_us = frames[index].timestamp_us;
    item.difference = normalized_mean_difference(
        frames[index - 1], frames[index]);
    if (index + 1 < frames.size()) {
      item.immediate_following_difference = normalized_mean_difference(
          frames[index], frames[index + 1]);
    }
    const std::size_t lookahead_end = std::min(
        frames.size() - 1,
        index + options.extended_stability_lookahead_frames);
    for (std::size_t future = index + 1;
         future <= lookahead_end; ++future) {
      item.minimum_lookahead_difference = std::min(
          item.minimum_lookahead_difference,
          normalized_mean_difference(frames[future - 1], frames[future]));
    }
    bool begins_sustained_transition = false;
    if (index + options.sustained_transition_observations - 1 <
        frames.size()) {
      begins_sustained_transition = true;
      for (std::size_t offset = 0;
           offset < options.sustained_transition_observations; ++offset) {
        const std::size_t right_index = index + offset;
        const double transition_difference = normalized_mean_difference(
            frames[right_index - 1], frames[right_index]);
        if (transition_difference <
            options.sustained_transition_difference_threshold) {
          begins_sustained_transition = false;
          break;
        }
      }
    }
    item.is_sustained_transition =
        begins_sustained_transition && !sustained_transition_in_progress;
    if (begins_sustained_transition) {
      sustained_transition_in_progress = true;
    } else if (item.difference <
               options.sustained_transition_difference_threshold) {
      sustained_transition_in_progress = false;
    }
    item.is_cut =
        (item.difference >= options.immediate_difference_threshold &&
         item.immediate_following_difference <=
             options.stable_difference_threshold) ||
        item.difference >= options.hard_cut_difference_threshold ||
        item.is_sustained_transition;
    evidence.push_back(item);
  }
  return evidence;
}

}  // namespace svp::vision
