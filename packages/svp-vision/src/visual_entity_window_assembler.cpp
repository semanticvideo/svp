#include "svp/vision/visual_entity_window_assembler.hpp"
#include "svp/vision/visual_entity_identity_reconciliation.hpp"

#include <algorithm>
#include <iomanip>
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
  if (options_.handoff_retention_us <= 0) {
    throw std::invalid_argument("handoff retention must be positive");
  }
  if (options_.maximum_identity_evidence_regions <
      options_.minimum_reacquisition_embedding_observations) {
    throw std::invalid_argument(
        "identity evidence retention must satisfy reacquisition");
  }
  if (!options_.artifact_sink && !options_.retain_artifacts_in_memory) {
    throw std::invalid_argument(
        "visual entity assembly requires a streaming artifact sink");
  }
}

std::vector<TrackedRegion> VisualEntityWindowAssembler::reconciliation_regions(
    const EntityState& state) const {
  std::vector<TrackedRegion> regions = state.identity_evidence;
  regions.insert(regions.end(), state.regions.begin(), state.regions.end());
  std::sort(regions.begin(), regions.end(),
            [](const TrackedRegion& left, const TrackedRegion& right) {
              if (left.timestamp_us != right.timestamp_us) {
                return left.timestamp_us < right.timestamp_us;
              }
              return left.region_id < right.region_id;
            });
  regions.erase(std::unique(regions.begin(), regions.end(),
                            [](const TrackedRegion& left,
                               const TrackedRegion& right) {
                              return left.region_id == right.region_id;
                            }),
                regions.end());
  return regions;
}

void VisualEntityWindowAssembler::remember_identity_evidence(
    EntityState& state,
    const TrackedRegion& region) {
  if ((region.detector_category_index < 0 || region.embedding.empty()) &&
      region.candidate_source != "motion_group") {
    return;
  }
  TrackedRegion evidence = region;
  evidence.mask_pixels.clear();
  evidence.mask_pixels.shrink_to_fit();
  state.identity_evidence.push_back(std::move(evidence));
  if (state.identity_evidence.size() >
      options_.maximum_identity_evidence_regions) {
    state.identity_evidence.erase(state.identity_evidence.begin());
  }
}

void VisualEntityWindowAssembler::emit_finalized_before(
    std::int64_t timestamp_us) {
  std::vector<TrackedRegion> regions;
  std::vector<MaskWriteEntry> masks;
  for (auto& [_, state] : entities_) {
    const auto retained = std::stable_partition(
        state.regions.begin(), state.regions.end(),
        [timestamp_us](const TrackedRegion& region) {
          return region.timestamp_us >= timestamp_us;
        });
    for (auto iterator = retained; iterator != state.regions.end(); ++iterator) {
      const auto mask = pending_masks_.find(iterator->mask_ref);
      if (mask != pending_masks_.end()) {
        masks.push_back(std::move(mask->second));
        pending_masks_.erase(mask);
      }
      regions.push_back(std::move(*iterator));
    }
    state.regions.erase(retained, state.regions.end());
  }
  if (regions.empty() && masks.empty()) return;
  if (options_.artifact_sink) {
    options_.artifact_sink(regions, masks);
  }
  if (options_.retain_artifacts_in_memory) {
    collected_regions_.insert(
        collected_regions_.end(),
        std::make_move_iterator(regions.begin()),
        std::make_move_iterator(regions.end()));
    collected_masks_.insert(
        collected_masks_.end(),
        std::make_move_iterator(masks.begin()),
        std::make_move_iterator(masks.end()));
  }
}

void VisualEntityWindowAssembler::append_window(
    EntityTrackResult window_result,
    const std::vector<std::int64_t>& sampled_timestamps_us,
    std::int64_t overlap_start_us,
    std::int64_t emit_after_us,
    const std::vector<std::int64_t>& discontinuity_timestamps_us) {
  for (const auto timestamp_us : sampled_timestamps_us) {
    if (timestamp_us > latest_sampled_timestamp_us_) {
      ++sampled_timestamp_count_;
      latest_sampled_timestamp_us_ = timestamp_us;
    }
  }

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
    const auto segment = std::upper_bound(
        discontinuity_timestamps_us.begin(),
        discontinuity_timestamps_us.end(),
        region.timestamp_us) - discontinuity_timestamps_us.begin();
    const std::string local_segment_id =
        region.entity_id + "#segment_" + std::to_string(segment);
    local_regions[local_segment_id].push_back(std::move(region));
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
  std::map<std::string, std::string> local_identity_parent;
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
      const double appearance =
          identity_reconciliation::supported_reacquisition_score(
          prior->second, current->second, options_);
      const double group =
          identity_reconciliation::motion_group_reacquisition_score(
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
    auto parent = local_identity_parent.find(best_prior_id);
    local_identity_parent[current_id] = parent == local_identity_parent.end()
        ? best_prior_id
        : parent->second;
  }

  struct CandidateMatch {
    std::string local_id;
    std::string global_id;
    double score = 0.0;
    bool continues_track = false;
  };
  std::vector<CandidateMatch> candidates;
  if (!entities_.empty()) {
    for (const auto& [local_id, regions] : local_regions) {
      for (const auto& [global_id, state] : entities_) {
        const double spatial_score = identity_reconciliation::overlap_score(
            state.regions, regions, overlap_start_us);
        if (spatial_score >= options_.minimum_overlap_iou) {
          candidates.push_back(
              {local_id, global_id, 2.0 + spatial_score, true});
          continue;
        }
        const bool separated_in_time = state.observation_count > 0 &&
            !regions.empty() &&
            state.last_seen_us < regions.front().timestamp_us;
        if (!separated_in_time) continue;
        const auto prior_regions = reconciliation_regions(state);
        const double visual_score =
            identity_reconciliation::supported_reacquisition_score(
            prior_regions, regions, options_);
        const double group_score =
            identity_reconciliation::motion_group_reacquisition_score(
            prior_regions, regions, options_);
        if (visual_score >= options_.minimum_supported_fragment_similarity) {
          candidates.push_back({local_id, global_id, visual_score, false});
        } else if (group_score >= options_.minimum_motion_group_endpoint_iou) {
          candidates.push_back(
              {local_id, global_id, 1.0 + group_score, false});
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
  std::map<std::string, bool> local_continues_track;
  std::set<std::string> assigned_global_ids;
  for (const auto& candidate : candidates) {
    if (local_to_global.count(candidate.local_id) > 0 ||
        assigned_global_ids.count(candidate.global_id) > 0) {
      continue;
    }
    local_to_global[candidate.local_id] = candidate.global_id;
    local_continues_track[candidate.local_id] = candidate.continues_track;
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
    bool continues_track = false;
    auto mapping = local_to_global.find(local_id);
    if (mapping != local_to_global.end()) {
      global_id = mapping->second;
      continues_track = local_continues_track.at(local_id);
    } else {
      const auto parent = local_identity_parent.find(local_id);
      if (parent != local_identity_parent.end()) {
        const auto parent_mapping = local_to_global.find(parent->second);
        if (parent_mapping != local_to_global.end()) {
          global_id = parent_mapping->second;
        }
      }
      if (global_id.empty()) {
        double best_visual_score = -1.0;
        for (const auto& [existing_id, state] : entities_) {
          if (state.observation_count == 0 ||
              state.last_seen_us >= regions.front().timestamp_us) {
            continue;
          }
          const auto prior_regions = reconciliation_regions(state);
          const double appearance =
              identity_reconciliation::supported_reacquisition_score(
              prior_regions, regions, options_);
          const double group =
              identity_reconciliation::motion_group_reacquisition_score(
              prior_regions, regions, options_);
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
      }
      if (global_id.empty()) {
        global_id = numbered_id("entity_", next_entity_index_);
        EntityState state;
        state.entity_id = global_id;
        entities_.emplace(global_id, std::move(state));
        ++next_entity_index_;
      }
    }
    local_to_global[local_id] = global_id;
    local_continues_track[local_id] = continues_track;

    auto& state = entities_.at(global_id);
    std::string output_track_id;
    for (auto& region : regions) {
      if (region.timestamp_us <= emit_after_us) continue;
      if (region.screen_area_ratio >= options_.maximum_entity_area_ratio &&
          region.candidate_source != "motion_group") {
        continue;
      }

      if (output_track_id.empty()) {
        if (continues_track && !state.track_ids.empty()) {
          output_track_id = state.track_ids.back();
        } else {
          output_track_id = numbered_id("track_", next_track_index_++);
          state.track_ids.push_back(output_track_id);
          TrackState track;
          track.track_id = output_track_id;
          track.reacquired = state.track_ids.size() > 1;
          state.tracks.emplace(output_track_id, std::move(track));
        }
      }

      region.entity_id = state.entity_id;
      region.track_id = output_track_id;
      region.region_id = numbered_id("region_", next_region_index_++);
      region.mask_ref = "mask_" + region.region_id;
      if (state.observation_count == 0) {
        state.first_seen_us = region.timestamp_us;
      }
      state.last_seen_us = region.timestamp_us;
      ++state.observation_count;
      state.screen_area_sum += region.screen_area_ratio;
      state.candidate_sources.insert(region.candidate_source);

      auto& track = state.tracks.at(output_track_id);
      if (track.region_count == 0) {
        track.start_us = region.timestamp_us;
        track.start_frame_id = region.frame_id;
      }
      track.end_us = region.timestamp_us;
      track.end_frame_id = region.frame_id;
      ++track.region_count;
      track.candidate_sources.insert(region.candidate_source);

      if (!region.mask_pixels.empty() &&
          region.mask_width > 0 && region.mask_height > 0) {
        MaskWriteEntry mask;
        mask.mask_id = region.mask_ref;
        mask.entity_id = state.entity_id;
        mask.track_id = output_track_id;
        mask.region_id = region.region_id;
        mask.frame_id = region.frame_id;
        mask.timestamp_us = region.timestamp_us;
        mask.width = region.mask_width;
        mask.height = region.mask_height;
        mask.rle_data = encode_mask_rle(
            region.mask_pixels.data(), region.mask_width, region.mask_height);
        pending_masks_.emplace(mask.mask_id, std::move(mask));
        region.mask_pixels.clear();
        region.mask_pixels.shrink_to_fit();
      }
      remember_identity_evidence(state, region);
      state.regions.push_back(std::move(region));
    }
  }

  if (!sampled_timestamps_us.empty()) {
    emit_finalized_before(
        sampled_timestamps_us.back() - options_.handoff_retention_us);
  }
}

AssembledVisualEntityResult VisualEntityWindowAssembler::finish() {
  emit_finalized_before(std::numeric_limits<std::int64_t>::max());

  AssembledVisualEntityResult result;
  result.tracker_result = std::move(provenance_);

  std::set<std::string> retained_entity_ids;
  for (auto& [entity_id, state] : entities_) {
    if (state.observation_count < options_.minimum_observation_count) {
      continue;
    }
    retained_entity_ids.insert(entity_id);

    EntityRecord entity;
    entity.entity_id = state.entity_id;
    entity.entity_type = "visual_entity";
    entity.first_seen_us = state.first_seen_us;
    entity.last_seen_us = state.last_seen_us;
    entity.track_ids = state.track_ids;
    entity.processor_id = result.tracker_result.processor_id;
    entity.average_visibility = sampled_timestamp_count_ == 0
        ? 0.0
        : static_cast<double>(state.observation_count) /
              static_cast<double>(sampled_timestamp_count_);
    entity.average_screen_area = state.screen_area_sum /
        static_cast<double>(state.observation_count);
    entity.evidence_sources.push_back({
        {"type", "visual_tracking"},
        {"method", "windowed_optical_flow_kalman"},
        {"region_count", state.observation_count}});
    result.tracker_result.entities.push_back(std::move(entity));

    for (const auto& track_id : state.track_ids) {
      const auto& track_state = state.tracks.at(track_id);
      TrackRecord track;
      track.track_id = track_id;
      track.entity_id = state.entity_id;
      track.start_us = track_state.start_us;
      track.end_us = track_state.end_us;
      track.start_frame_id = track_state.start_frame_id;
      track.end_frame_id = track_state.end_frame_id;
      track.region_count = static_cast<int>(track_state.region_count);
      track.reacquired = track_state.reacquired;
      track.confidence = std::min(
          1.0, sampled_timestamp_count_ == 0
              ? 0.0
              : static_cast<double>(track_state.region_count) /
                    static_cast<double>(sampled_timestamp_count_));
      track.processor_id = result.tracker_result.processor_id;
      track.tracking_method = "windowed_optical_flow_kalman";
      track.candidate_source =
          combined_candidate_source(track_state.candidate_sources);
      result.tracker_result.tracks.push_back(std::move(track));
    }
  }

  if (options_.retain_artifacts_in_memory) {
    for (auto& region : collected_regions_) {
      if (retained_entity_ids.count(region.entity_id) > 0) {
        result.tracker_result.regions.push_back(std::move(region));
      }
    }
    for (auto& mask : collected_masks_) {
      if (retained_entity_ids.count(mask.entity_id) > 0) {
        result.masks.push_back(std::move(mask));
      }
    }
  }
  return result;
}

}  // namespace svp::vision
