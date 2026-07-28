#include "svp/vision/visual_entity_identity_reconciliation.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace svp::vision::identity_reconciliation {
namespace {

double box_iou(const TrackedRegion& left, const TrackedRegion& right) {
  const double x0 = std::max(left.box_norm[0], right.box_norm[0]);
  const double y0 = std::max(left.box_norm[1], right.box_norm[1]);
  const double x1 = std::min(left.box_norm[2], right.box_norm[2]);
  const double y1 = std::min(left.box_norm[3], right.box_norm[3]);
  const double intersection =
      std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
  const double left_area =
      std::max(0.0, left.box_norm[2] - left.box_norm[0]) *
      std::max(0.0, left.box_norm[3] - left.box_norm[1]);
  const double right_area =
      std::max(0.0, right.box_norm[2] - right.box_norm[0]) *
      std::max(0.0, right.box_norm[3] - right.box_norm[1]);
  const double union_area = left_area + right_area - intersection;
  return union_area > 0.0 ? intersection / union_area : 0.0;
}

double cosine_similarity(
    const std::vector<float>& left,
    const std::vector<float>& right) {
  if (left.empty() || left.size() != right.size()) return -1.0;
  double dot = 0.0;
  double left_norm = 0.0;
  double right_norm = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    dot += static_cast<double>(left[index]) * right[index];
    left_norm += static_cast<double>(left[index]) * left[index];
    right_norm += static_cast<double>(right[index]) * right[index];
  }
  if (left_norm <= 0.0 || right_norm <= 0.0) return -1.0;
  return dot / (std::sqrt(left_norm) * std::sqrt(right_norm));
}

bool is_detector_backed(const TrackedRegion& region) {
  return region.candidate_source.find("detector") != std::string::npos ||
      region.candidate_source == "objectness_detector";
}

bool is_motion_group(const std::vector<TrackedRegion>& regions) {
  return !regions.empty() &&
      std::all_of(regions.begin(), regions.end(), [](const auto& region) {
        return region.candidate_source == "motion_group";
      });
}

double appearance_score(
    const std::vector<TrackedRegion>& previous,
    const std::vector<TrackedRegion>& current,
    std::size_t minimum_previous_observations) {
  double best = -1.0;
  const std::size_t previous_embedding_count = std::count_if(
      previous.begin(), previous.end(), [](const TrackedRegion& region) {
        return is_detector_backed(region) && !region.embedding.empty();
      });
  if (previous_embedding_count < minimum_previous_observations) return best;
  for (const auto& right : current) {
    if (!is_detector_backed(right) || right.embedding.empty()) continue;
    std::vector<double> similarities;
    for (const auto& left : previous) {
      if (!is_detector_backed(left) || left.embedding.empty()) continue;
      if (left.detector_category_index >= 0 &&
          right.detector_category_index >= 0 &&
          left.detector_category_index != right.detector_category_index) {
        continue;
      }
      similarities.push_back(cosine_similarity(left.embedding, right.embedding));
    }
    if (similarities.size() < minimum_previous_observations) continue;
    std::sort(similarities.begin(), similarities.end(), std::greater<>());
    // The weakest value among the required strongest observations is the
    // consensus score. One coincidental crop cannot carry the merge.
    best = std::max(
        best, similarities[minimum_previous_observations - 1]);
  }
  return best;
}

double mean_screen_area(const std::vector<TrackedRegion>& regions) {
  if (regions.empty()) return 0.0;
  double total = 0.0;
  for (const auto& region : regions) total += region.screen_area_ratio;
  return total / static_cast<double>(regions.size());
}

double area_similarity(
    const std::vector<TrackedRegion>& left,
    const std::vector<TrackedRegion>& right) {
  const double left_area = mean_screen_area(left);
  const double right_area = mean_screen_area(right);
  const double larger = std::max(left_area, right_area);
  return larger > 0.0 ? std::min(left_area, right_area) / larger : 0.0;
}

}  // namespace

double overlap_score(
    const std::vector<TrackedRegion>& previous,
    const std::vector<TrackedRegion>& current,
    std::int64_t overlap_start_us) {
  double score_sum = 0.0;
  std::size_t matches = 0;
  for (const auto& current_region : current) {
    if (current_region.timestamp_us < overlap_start_us) continue;
    double best_at_timestamp = 0.0;
    for (const auto& previous_region : previous) {
      if (previous_region.timestamp_us != current_region.timestamp_us) continue;
      best_at_timestamp =
          std::max(best_at_timestamp, box_iou(previous_region, current_region));
    }
    if (best_at_timestamp > 0.0) {
      score_sum += best_at_timestamp;
      ++matches;
    }
  }
  return matches > 0 ? score_sum / static_cast<double>(matches) : 0.0;
}

double motion_group_reacquisition_score(
    const std::vector<TrackedRegion>& previous,
    const std::vector<TrackedRegion>& current,
    const VisualEntityWindowAssemblerOptions& options) {
  if (!is_motion_group(previous) || !is_motion_group(current) ||
      current.front().timestamp_us <= previous.back().timestamp_us ||
      current.front().timestamp_us - previous.back().timestamp_us >
          options.maximum_motion_group_gap_us) {
    return -1.0;
  }
  const double endpoint_iou = box_iou(previous.back(), current.front());
  return endpoint_iou >= options.minimum_motion_group_endpoint_iou
      ? endpoint_iou
      : -1.0;
}

double supported_reacquisition_score(
    const std::vector<TrackedRegion>& previous,
    const std::vector<TrackedRegion>& current,
    const VisualEntityWindowAssemblerOptions& options) {
  const double forward = appearance_score(
      previous, current,
      options.minimum_reacquisition_embedding_observations);
  if (area_similarity(previous, current) <
      options.minimum_supported_fragment_area_similarity) {
    return -1.0;
  }
  if (forward >= options.minimum_reacquisition_similarity) return forward;
  if (previous.empty() || current.empty() ||
      current.front().timestamp_us - previous.back().timestamp_us <
          options.minimum_supported_fragment_gap_us) {
    return -1.0;
  }
  const double reverse = appearance_score(
      current, previous,
      options.minimum_reacquisition_embedding_observations);
  const double consensus = std::min(forward, reverse);
  if (consensus < options.minimum_supported_fragment_similarity) {
    return -1.0;
  }
  return consensus;
}

}  // namespace svp::vision::identity_reconciliation
