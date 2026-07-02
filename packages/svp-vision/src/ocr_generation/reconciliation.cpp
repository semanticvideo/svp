#include "ocr_generation_internal.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <map>

namespace svp::vision::ocr_generation_internal {
namespace {

double bbox_iou(int a_left,
                int a_top,
                int a_right,
                int a_bottom,
                int b_left,
                int b_top,
                int b_right,
                int b_bottom) {
  const int inter_left = std::max(a_left, b_left);
  const int inter_top = std::max(a_top, b_top);
  const int inter_right = std::min(a_right, b_right);
  const int inter_bottom = std::min(a_bottom, b_bottom);
  if (inter_right <= inter_left || inter_bottom <= inter_top) return 0.0;
  const double inter_area =
      static_cast<double>(inter_right - inter_left) *
      static_cast<double>(inter_bottom - inter_top);
  const double a_area =
      static_cast<double>(a_right - a_left) *
      static_cast<double>(a_bottom - a_top);
  const double b_area =
      static_cast<double>(b_right - b_left) *
      static_cast<double>(b_bottom - b_top);
  const double union_area = a_area + b_area - inter_area;
  return union_area > 0.0 ? inter_area / union_area : 0.0;
}

}  // namespace

std::vector<ReconciledObservation> reconcile_detections(
    const std::vector<FrameDetection>& detections,
    int total_frames,
    double min_confidence,
    std::size_t min_text_chars) {
  std::vector<ReconciledObservation> reconciled;

  std::map<std::string, std::vector<std::size_t>> by_text_key;
  for (std::size_t i = 0; i < detections.size(); ++i) {
    const std::string norm = normalize_text(detections[i].raw_text);
    const std::string key = alphanumeric_key(norm);
    if (key.length() < min_text_chars) continue;
    by_text_key[key].push_back(i);
  }

  for (const auto& [key, indices] : by_text_key) {
    std::vector<std::vector<std::size_t>> clusters;
    for (std::size_t idx : indices) {
      bool added = false;
      for (auto& cluster : clusters) {
        for (std::size_t cidx : cluster) {
          const double iou = bbox_iou(
              detections[idx].bbox_left, detections[idx].bbox_top,
              detections[idx].bbox_right, detections[idx].bbox_bottom,
              detections[cidx].bbox_left, detections[cidx].bbox_top,
              detections[cidx].bbox_right, detections[cidx].bbox_bottom);
          if (iou > 0.3) {
            cluster.push_back(idx);
            added = true;
            break;
          }
        }
        if (added) break;
      }
      if (!added) clusters.push_back({idx});
    }

    for (const auto& cluster : clusters) {
      ReconciledObservation obs;
      std::size_t best_idx = cluster[0];
      for (std::size_t idx : cluster) {
        if (detections[idx].raw_text.size() > detections[best_idx].raw_text.size()) {
          best_idx = idx;
        }
      }
      obs.raw_text = detections[best_idx].raw_text;
      obs.normalized_text = normalize_text(obs.raw_text);
      obs.frame_width = detections[cluster[0]].frame_width;
      obs.frame_height = detections[cluster[0]].frame_height;
      obs.detection_count = static_cast<int>(cluster.size());

      double conf_sum = 0.0;
      obs.bbox_left = INT_MAX;
      obs.bbox_top = INT_MAX;
      obs.bbox_right = 0;
      obs.bbox_bottom = 0;
      obs.start_us = INT64_MAX;
      obs.end_us = 0;
      obs.frame_start = SIZE_MAX;
      obs.frame_end = 0;

      for (std::size_t idx : cluster) {
        const auto& det = detections[idx];
        conf_sum += det.confidence;
        obs.bbox_left = std::min(obs.bbox_left, det.bbox_left);
        obs.bbox_top = std::min(obs.bbox_top, det.bbox_top);
        obs.bbox_right = std::max(obs.bbox_right, det.bbox_right);
        obs.bbox_bottom = std::max(obs.bbox_bottom, det.bbox_bottom);
        obs.start_us = std::min(obs.start_us, det.timestamp_us);
        obs.end_us = std::max(obs.end_us, det.timestamp_us);
        obs.frame_start = std::min(obs.frame_start, det.frame_index);
        obs.frame_end = std::max(obs.frame_end, det.frame_index);
        obs.source_frame_ids.push_back(det.frame_id);
      }

      obs.confidence = conf_sum / static_cast<double>(cluster.size());
      if (total_frames > 1 && cluster.size() == 1 && obs.confidence < min_confidence) {
        continue;
      }
      if (cluster.size() > 1) {
        obs.confidence = std::min(1.0, obs.confidence + 0.1 * (cluster.size() - 1));
      }
      reconciled.push_back(std::move(obs));
    }
  }

  std::sort(reconciled.begin(), reconciled.end(),
            [](const ReconciledObservation& a, const ReconciledObservation& b) {
              return a.start_us < b.start_us;
            });

  return reconciled;
}

}  // namespace svp::vision::ocr_generation_internal
