#include "svp/vision/visual_entity_window_assembler.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace svp::vision {
namespace {

std::string numbered_id(const char* prefix, std::size_t index) {
  std::ostringstream stream;
  stream << prefix << std::setfill('0') << std::setw(6) << index;
  return stream.str();
}

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

std::string combined_candidate_source(const std::set<std::string>& sources) {
  if (sources.count("motion_group") > 0) return "motion_group";
  if (sources.count("fused_motion_depth") > 0 ||
      (sources.count("motion") > 0 && sources.count("depth") > 0)) {
    return "fused_motion_depth";
  }
  if (sources.empty()) return "motion";
  return *sources.begin();
}

}  // namespace

VisualEntityWindowAssembler::VisualEntityWindowAssembler(
    VisualEntityWindowAssemblerOptions options)
    : options_(options) {
  if (options_.minimum_overlap_iou <= 0.0 ||
      options_.minimum_overlap_iou > 1.0) {
    throw std::invalid_argument("overlap IoU must be in (0, 1]");
  }
  if (options_.maximum_entity_area_ratio <= 0.0 ||
      options_.maximum_entity_area_ratio > 1.0) {
    throw std::invalid_argument("maximum entity area ratio must be in (0, 1]");
  }
  if (options_.minimum_observation_count < 2) {
    throw std::invalid_argument(
        "persistent entities require at least two observations");
  }
  if (options_.minimum_reacquisition_similarity <= 0.0 ||
      options_.minimum_reacquisition_similarity > 1.0) {
    throw std::invalid_argument(
        "reacquisition similarity must be in (0, 1]");
  }
  if (options_.minimum_reacquisition_embedding_observations < 2) {
    throw std::invalid_argument(
        "reacquisition requires repeated appearance observations");
  }
  if (options_.minimum_supported_fragment_similarity <= 0.0 ||
      options_.minimum_supported_fragment_similarity > 1.0 ||
      options_.minimum_supported_fragment_area_similarity <= 0.0 ||
      options_.minimum_supported_fragment_area_similarity > 1.0 ||
      options_.minimum_supported_fragment_gap_us < 0 ||
      options_.maximum_motion_group_gap_us < 0 ||
      options_.minimum_motion_group_endpoint_iou <= 0.0 ||
      options_.minimum_motion_group_endpoint_iou > 1.0) {
    throw std::invalid_argument(
        "local fragment evidence thresholds must be in (0, 1]");
  }
}

void VisualEntityWindowAssembler::append_window(
    EntityTrackResult window_result,
    const std::vector<std::int64_t>& sampled_timestamps_us,
    std::int64_t overlap_start_us,
    std::int64_t emit_after_us) {
  sampled_timestamps_us_.insert(
      sampled_timestamps_us.begin(), sampled_timestamps_us.end());

  if (provenance_.processor_id.empty()) {
    provenance_.processor_id = window_result.processor_id;
    provenance_.model_refs = window_result.model_refs;
    provenance_.runtime = window_result.runtime;
    provenance_.execution_provider = window_result.execution_provider;
    provenance_.confidence_calibration_status =
        window_result.confidence_calibration_status;
    provenance_.limitations_note = window_result.limitations_note;
    provenance_.opencv_version = window_result.opencv_version;
    provenance_.parameters_json = window_result.parameters_json;
  }

  std::map<std::string, std::vector<TrackedRegion>> local_regions;
  for (auto& region : window_result.regions) {
    local_regions[region.entity_id].push_back(std::move(region));
  }

  // A hard cut can outlast the tracker's bounded lost-frame lifetime while
  // both appearances still occur inside one decode window. Reconcile those
  // local fragments before window-to-window handoff. Agreement must be
  // bidirectional so repeated evidence exists on both sides of the cut.
  std::vector<std::string> chronological_local_ids;
  chronological_local_ids.reserve(local_regions.size());
  for (const auto& [local_id, _] : local_regions) {
    chronological_local_ids.push_back(local_id);
  }
  std::sort(
      chronological_local_ids.begin(), chronological_local_ids.end(),
      [&](const std::string& left, const std::string& right) {
        const auto& left_regions = local_regions.at(left);
        const auto& right_regions = local_regions.at(right);
        if (left_regions.front().timestamp_us !=
            right_regions.front().timestamp_us) {
          return left_regions.front().timestamp_us <
              right_regions.front().timestamp_us;
        }
        return left < right;
      });
  for (std::size_t current_index = 0;
       current_index < chronological_local_ids.size(); ++current_index) {
    const auto& current_id = chronological_local_ids[current_index];
    auto current = local_regions.find(current_id);
    if (current == local_regions.end() || current->second.empty()) continue;
    std::string best_prior_id;
    double best_score = -1.0;
    for (std::size_t prior_index = 0; prior_index < current_index;
         ++prior_index) {
      const auto& prior_id = chronological_local_ids[prior_index];
      auto prior = local_regions.find(prior_id);
      if (prior == local_regions.end() || prior->second.empty() ||
          prior->second.back().timestamp_us >=
              current->second.front().timestamp_us) {
        continue;
      }
      const double appearance = supported_reacquisition_score(
          prior->second, current->second, options_);
      const double group = motion_group_reacquisition_score(
          prior->second, current->second, options_);
      const double score = std::max(
          appearance,
          group >= options_.minimum_motion_group_endpoint_iou
              ? 1.0 + group
              : -1.0);
      if (score >= options_.minimum_supported_fragment_similarity &&
          (score > best_score ||
           (score == best_score && prior_id < best_prior_id))) {
        best_score = score;
        best_prior_id = prior_id;
      }
    }
    if (best_prior_id.empty()) continue;
    auto& destination = local_regions.at(best_prior_id);
    destination.insert(
        destination.end(),
        std::make_move_iterator(current->second.begin()),
        std::make_move_iterator(current->second.end()));
    std::sort(destination.begin(), destination.end(),
              [](const TrackedRegion& left, const TrackedRegion& right) {
                if (left.timestamp_us != right.timestamp_us) {
                  return left.timestamp_us < right.timestamp_us;
                }
                return left.region_id < right.region_id;
              });
    local_regions.erase(current);
  }

  struct CandidateMatch {
    std::string local_id;
    std::string global_id;
    double score = 0.0;
  };
  std::vector<CandidateMatch> candidates;
  if (!entities_.empty()) {
    for (const auto& [local_id, regions] : local_regions) {
      for (const auto& [global_id, state] : entities_) {
        const double spatial_score =
            overlap_score(state.regions, regions, overlap_start_us);
        if (spatial_score >= options_.minimum_overlap_iou) {
          candidates.push_back({local_id, global_id, 2.0 + spatial_score});
          continue;
        }
        const bool separated_in_time = !state.regions.empty() &&
            !regions.empty() &&
            state.regions.back().timestamp_us < regions.front().timestamp_us;
        if (!separated_in_time) continue;
        const double visual_score = supported_reacquisition_score(
            state.regions, regions, options_);
        const double group_score = motion_group_reacquisition_score(
            state.regions, regions, options_);
        if (visual_score >= options_.minimum_supported_fragment_similarity) {
          candidates.push_back({local_id, global_id, visual_score});
        } else if (group_score >= options_.minimum_motion_group_endpoint_iou) {
          candidates.push_back({local_id, global_id, 1.0 + group_score});
        }
      }
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const CandidateMatch& left, const CandidateMatch& right) {
              if (left.score != right.score) return left.score > right.score;
              if (left.local_id != right.local_id) {
                return left.local_id < right.local_id;
              }
              return left.global_id < right.global_id;
            });

  std::map<std::string, std::string> local_to_global;
  std::set<std::string> assigned_global_ids;
  for (const auto& candidate : candidates) {
    if (local_to_global.count(candidate.local_id) > 0 ||
        assigned_global_ids.count(candidate.global_id) > 0) {
      continue;
    }
    local_to_global[candidate.local_id] = candidate.global_id;
    assigned_global_ids.insert(candidate.global_id);
  }

  std::vector<std::string> local_order;
  local_order.reserve(local_regions.size());
  for (const auto& [local_id, _] : local_regions) {
    local_order.push_back(local_id);
  }
  std::sort(local_order.begin(), local_order.end(),
            [&](const std::string& left, const std::string& right) {
              const auto& left_regions = local_regions.at(left);
              const auto& right_regions = local_regions.at(right);
              if (left_regions.front().timestamp_us !=
                  right_regions.front().timestamp_us) {
                return left_regions.front().timestamp_us <
                    right_regions.front().timestamp_us;
              }
              return left < right;
            });

  for (const auto& local_id : local_order) {
    auto& regions = local_regions.at(local_id);
    std::string global_id;
    auto mapping = local_to_global.find(local_id);
    if (mapping != local_to_global.end()) {
      global_id = mapping->second;
    } else {
      double best_visual_score = -1.0;
      for (const auto& [existing_id, state] : entities_) {
        if (state.regions.empty() ||
            state.regions.back().timestamp_us >=
                regions.front().timestamp_us) {
          continue;
        }
        const double appearance = supported_reacquisition_score(
            state.regions, regions, options_);
        const double group = motion_group_reacquisition_score(
            state.regions, regions, options_);
        const double score = std::max(
            appearance,
            group >= options_.minimum_motion_group_endpoint_iou
                ? 1.0 + group
                : -1.0);
        if (score >= options_.minimum_supported_fragment_similarity &&
            (score > best_visual_score ||
             (score == best_visual_score && existing_id < global_id))) {
          best_visual_score = score;
          global_id = existing_id;
        }
      }
      if (global_id.empty()) {
        global_id = numbered_id("entity_", next_entity_index_);
        EntityState state;
        state.entity_id = global_id;
        state.track_id = numbered_id("track_", next_entity_index_);
        entities_.emplace(global_id, std::move(state));
        ++next_entity_index_;
      }
    }

    auto& state = entities_.at(global_id);
    for (auto& region : regions) {
      if (region.timestamp_us <= emit_after_us) continue;
      if (region.screen_area_ratio >= options_.maximum_entity_area_ratio &&
          region.candidate_source != "motion_group") {
        continue;
      }

      region.entity_id = state.entity_id;
      region.track_id = state.track_id;
      region.region_id = numbered_id("region_", next_region_index_++);
      region.mask_ref = "mask_" + region.region_id;
      state.observation_times_us.insert(region.timestamp_us);
      state.candidate_sources.insert(region.candidate_source);

      if (!region.mask_pixels.empty() &&
          region.mask_width > 0 && region.mask_height > 0) {
        MaskWriteEntry mask;
        mask.mask_id = region.mask_ref;
        mask.entity_id = state.entity_id;
        mask.track_id = state.track_id;
        mask.region_id = region.region_id;
        mask.frame_id = region.frame_id;
        mask.timestamp_us = region.timestamp_us;
        mask.width = region.mask_width;
        mask.height = region.mask_height;
        mask.rle_data = encode_mask_rle(
            region.mask_pixels.data(), region.mask_width, region.mask_height);
        masks_.push_back(std::move(mask));
        region.mask_pixels.clear();
        region.mask_pixels.shrink_to_fit();
      }
      state.regions.push_back(std::move(region));
    }
  }
}

AssembledVisualEntityResult VisualEntityWindowAssembler::finish() {
  AssembledVisualEntityResult result;
  result.tracker_result = std::move(provenance_);

  std::set<std::string> retained_mask_ids;
  for (auto& [entity_id, state] : entities_) {
    if (state.observation_times_us.size() < options_.minimum_observation_count) {
      continue;
    }

    std::sort(state.regions.begin(), state.regions.end(),
              [](const TrackedRegion& left, const TrackedRegion& right) {
                if (left.timestamp_us != right.timestamp_us) {
                  return left.timestamp_us < right.timestamp_us;
                }
                return left.region_id < right.region_id;
              });
    if (state.regions.empty()) continue;

    EntityRecord entity;
    entity.entity_id = state.entity_id;
    entity.entity_type = state.candidate_sources.count("motion_group") > 0
        ? "dynamic_group"
        : "visual_entity";
    entity.first_seen_us = state.regions.front().timestamp_us;
    entity.last_seen_us = state.regions.back().timestamp_us;
    entity.track_ids = {state.track_id};
    entity.processor_id = result.tracker_result.processor_id;
    double area_sum = 0.0;
    for (const auto& region : state.regions) {
      area_sum += region.screen_area_ratio;
      retained_mask_ids.insert(region.mask_ref);
    }
    entity.average_visibility = sampled_timestamps_us_.empty()
        ? 0.0
        : static_cast<double>(state.observation_times_us.size()) /
              static_cast<double>(sampled_timestamps_us_.size());
    entity.average_screen_area =
        area_sum / static_cast<double>(state.regions.size());
    entity.evidence_sources.push_back({
        {"type", "visual_tracking"},
        {"method", "windowed_optical_flow_kalman"},
        {"region_count", state.regions.size()}});
    result.tracker_result.entities.push_back(std::move(entity));

    TrackRecord track;
    track.track_id = state.track_id;
    track.entity_id = state.entity_id;
    track.start_us = state.regions.front().timestamp_us;
    track.end_us = state.regions.back().timestamp_us;
    track.start_frame_id = state.regions.front().frame_id;
    track.end_frame_id = state.regions.back().frame_id;
    track.region_count = static_cast<int>(state.regions.size());
    track.confidence = std::min(
        1.0, static_cast<double>(state.observation_times_us.size()) /
                 static_cast<double>(sampled_timestamps_us_.size()));
    track.processor_id = result.tracker_result.processor_id;
    track.tracking_method = "windowed_optical_flow_kalman";
    track.candidate_source = combined_candidate_source(state.candidate_sources);
    result.tracker_result.tracks.push_back(std::move(track));

    for (auto& region : state.regions) {
      result.tracker_result.regions.push_back(std::move(region));
    }
  }

  for (auto& mask : masks_) {
    if (retained_mask_ids.count(mask.mask_id) > 0) {
      result.masks.push_back(std::move(mask));
    }
  }
  return result;
}

}  // namespace svp::vision
