#include "private.hpp"

#include "svp/core/memory_diagnostics.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>

namespace svp::audio::sherpa_diarization_internal {

std::map<int32_t, int32_t> cluster_speaker_observations(
    const std::vector<SpeakerObservation>& observations,
    const std::set<std::pair<int32_t, int32_t>>& cannot_link_observations) {
  std::map<int32_t, int32_t> observation_to_final;
  if (observations.empty()) return observation_to_final;

  std::vector<std::vector<std::size_t>> clusters;
  clusters.reserve(observations.size());
  for (std::size_t i = 0; i < observations.size(); ++i) {
    clusters.push_back({i});
  }

  auto clusters_can_merge = [&](const std::vector<std::size_t>& a,
                                const std::vector<std::size_t>& b) {
    for (std::size_t ai : a) {
      for (std::size_t bi : b) {
        const auto cannot_link = std::minmax(observations[ai].observation_id,
                                             observations[bi].observation_id);
        if (cannot_link_observations.find(cannot_link) !=
            cannot_link_observations.end()) {
          return false;
        }
      }
    }
    return true;
  };

  auto cluster_similarity = [&](const std::vector<std::size_t>& a,
                                const std::vector<std::size_t>& b) {
    if (!clusters_can_merge(a, b)) return -2.0f;
    float total = 0.0f;
    std::size_t count = 0;
    for (std::size_t ai : a) {
      if (!has_embedding_signal(observations[ai].embedding)) continue;
      for (std::size_t bi : b) {
        if (!has_embedding_signal(observations[bi].embedding)) continue;
        total += cosine_similarity(observations[ai].embedding,
                                   observations[bi].embedding);
        ++count;
      }
    }
    return count == 0 ? -2.0f : total / static_cast<float>(count);
  };

  std::vector<std::tuple<float, int32_t, int32_t>> observation_pairs;
  for (std::size_t i = 0; i < observations.size(); ++i) {
    if (!has_embedding_signal(observations[i].embedding)) continue;
    for (std::size_t j = i + 1; j < observations.size(); ++j) {
      if (!has_embedding_signal(observations[j].embedding)) continue;
      observation_pairs.push_back({
          cosine_similarity(observations[i].embedding, observations[j].embedding),
          observations[i].observation_id,
          observations[j].observation_id});
    }
  }
  std::sort(observation_pairs.begin(), observation_pairs.end(),
            [](const auto& a, const auto& b) {
              return std::get<0>(a) > std::get<0>(b);
            });
  std::string top_pairs;
  const std::size_t pair_count =
      std::min<std::size_t>(observation_pairs.size(), 16);
  for (std::size_t i = 0; i < pair_count; ++i) {
    if (i > 0) top_pairs += ";";
    top_pairs += "sim=" + std::to_string(std::get<0>(observation_pairs[i])) +
                 ",a=" + std::to_string(std::get<1>(observation_pairs[i])) +
                 ",b=" + std::to_string(std::get<2>(observation_pairs[i]));
  }
  svp::core::trace_memory_event("diarization.global_reconciliation.pairs", {
      {"observation_count", std::to_string(observations.size())},
      {"top_pairs", top_pairs}
  });

  struct MergeStep {
    std::vector<std::vector<std::size_t>> clusters_after;
    float similarity = -2.0f;
  };
  std::vector<MergeStep> merge_steps;

  while (clusters.size() > 1) {
    float best_similarity = -2.0f;
    std::size_t best_a = 0;
    std::size_t best_b = 0;
    for (std::size_t i = 0; i < clusters.size(); ++i) {
      for (std::size_t j = i + 1; j < clusters.size(); ++j) {
        const float similarity = cluster_similarity(clusters[i], clusters[j]);
        if (similarity > best_similarity) {
          best_similarity = similarity;
          best_a = i;
          best_b = j;
        }
      }
    }
    if (best_similarity < kGlobalSpeakerObservationFloorSimilarity) break;

    clusters[best_a].insert(clusters[best_a].end(),
                            clusters[best_b].begin(),
                            clusters[best_b].end());
    clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(best_b));
    merge_steps.push_back({clusters, best_similarity});
  }

  if (merge_steps.size() > 1) {
    float largest_gap = 0.0f;
    std::size_t selected_step = merge_steps.size() - 1;
    std::vector<std::pair<float, std::size_t>> gap_candidates;
    for (std::size_t i = 0; i + 1 < merge_steps.size(); ++i) {
      const float gap = merge_steps[i].similarity - merge_steps[i + 1].similarity;
      gap_candidates.push_back({gap, i});
      if (gap > largest_gap) {
        largest_gap = gap;
        selected_step = i;
      }
    }
    std::sort(gap_candidates.begin(), gap_candidates.end(),
              [](const auto& a, const auto& b) {
                return a.first > b.first;
              });
    std::string top_gaps;
    const std::size_t gap_count = std::min<std::size_t>(gap_candidates.size(), 8);
    for (std::size_t i = 0; i < gap_count; ++i) {
      if (i > 0) top_gaps += ";";
      const std::size_t step = gap_candidates[i].second;
      top_gaps += "gap=" + std::to_string(gap_candidates[i].first) +
                  ",clusters=" +
                  std::to_string(merge_steps[step].clusters_after.size()) +
                  ",sim=" + std::to_string(merge_steps[step].similarity);
    }
    const bool gap_cut_applied = largest_gap >= kGlobalSpeakerObservationMinGap;
    svp::core::trace_memory_event("diarization.global_reconciliation.gaps", {
        {"observation_count", std::to_string(observations.size())},
        {"selected_gap", std::to_string(largest_gap)},
        {"selected_clusters", std::to_string(merge_steps[selected_step].clusters_after.size())},
        {"floor_clusters", std::to_string(clusters.size())},
        {"gap_cut_applied", gap_cut_applied ? "true" : "false"},
        {"top_gaps", top_gaps}
    });
    if (gap_cut_applied) {
      clusters = merge_steps[selected_step].clusters_after;
    }
  }

  std::sort(clusters.begin(), clusters.end(),
            [&](const std::vector<std::size_t>& a,
                const std::vector<std::size_t>& b) {
              float a_start = observations[a.front()].first_start_sec;
              float b_start = observations[b.front()].first_start_sec;
              for (std::size_t idx : a) {
                a_start = std::min(a_start, observations[idx].first_start_sec);
              }
              for (std::size_t idx : b) {
                b_start = std::min(b_start, observations[idx].first_start_sec);
              }
              return a_start < b_start;
            });

  for (std::size_t final_id = 0; final_id < clusters.size(); ++final_id) {
    for (std::size_t obs_index : clusters[final_id]) {
      observation_to_final[observations[obs_index].observation_id] =
          static_cast<int32_t>(final_id);
    }
  }
  return observation_to_final;
}

struct SpeakerTrackStats {
  std::int64_t speech_us = 0;
  float first_start_sec = 0.0f;
  float last_end_sec = 0.0f;
  bool seen = false;
};

std::vector<std::vector<float>> build_reconciled_speaker_embeddings(
    const std::vector<SpeakerObservation>& observations,
    const std::vector<SherpaDiarizationSegment>& preliminary_segments,
    const std::vector<SherpaDiarizationSegment>& reconciled_segments,
    int32_t final_speaker_count) {
  std::map<int32_t, int32_t> observation_to_current_speaker;
  for (std::size_t index = 0;
       index < preliminary_segments.size() && index < reconciled_segments.size();
       ++index) {
    observation_to_current_speaker[preliminary_segments[index].speaker_id] =
        reconciled_segments[index].speaker_id;
  }

  std::vector<std::vector<float>> embeddings(
      static_cast<std::size_t>(std::max(0, final_speaker_count)));
  std::vector<int32_t> counts(embeddings.size(), 0);
  for (const auto& observation : observations) {
    const auto current =
        observation_to_current_speaker.find(observation.observation_id);
    if (current == observation_to_current_speaker.end() ||
        current->second < 0 || current->second >= final_speaker_count ||
        !has_embedding_signal(observation.embedding)) {
      continue;
    }
    auto& prototype = embeddings[static_cast<std::size_t>(current->second)];
    if (prototype.empty()) prototype.assign(observation.embedding.size(), 0.0f);
    if (prototype.size() != observation.embedding.size()) continue;
    for (std::size_t dimension = 0;
         dimension < observation.embedding.size(); ++dimension) {
      prototype[dimension] += observation.embedding[dimension];
    }
    ++counts[static_cast<std::size_t>(current->second)];
  }
  for (std::size_t speaker = 0; speaker < embeddings.size(); ++speaker) {
    if (counts[speaker] <= 0) continue;
    for (float& value : embeddings[speaker]) {
      value /= static_cast<float>(counts[speaker]);
    }
    normalize_embedding(embeddings[speaker]);
  }
  return embeddings;
}

void stitch_dominant_non_overlapping_tracks(
    std::vector<SherpaDiarizationSegment>& segments,
    int32_t& final_speaker_count,
    std::size_t observation_count) {
  if (final_speaker_count < kDominantStitchMinFinalSpeakers ||
      observation_count < kDominantStitchMinObservations ||
      segments.empty()) {
    return;
  }

  std::vector<SpeakerTrackStats> stats(static_cast<std::size_t>(final_speaker_count));
  std::int64_t total_speech_us = 0;
  for (const auto& seg : segments) {
    if (seg.speaker_id < 0 || seg.speaker_id >= final_speaker_count) continue;
    auto& st = stats[static_cast<std::size_t>(seg.speaker_id)];
    const std::int64_t dur_us = static_cast<std::int64_t>(
        std::max(0.0f, seg.end_sec - seg.start_sec) * 1000000.0f);
    st.speech_us += dur_us;
    total_speech_us += dur_us;
    if (!st.seen) {
      st.first_start_sec = seg.start_sec;
      st.last_end_sec = seg.end_sec;
      st.seen = true;
    } else {
      st.first_start_sec = std::min(st.first_start_sec, seg.start_sec);
      st.last_end_sec = std::max(st.last_end_sec, seg.end_sec);
    }
  }
  if (total_speech_us <= 0) return;

  std::vector<int32_t> speakers;
  for (int32_t sid = 0; sid < final_speaker_count; ++sid) {
    if (stats[static_cast<std::size_t>(sid)].seen) speakers.push_back(sid);
  }
  std::sort(speakers.begin(), speakers.end(),
            [&](int32_t a, int32_t b) {
              return stats[static_cast<std::size_t>(a)].speech_us >
                     stats[static_cast<std::size_t>(b)].speech_us;
            });
  if (speakers.size() < 2) return;

  const int32_t a = speakers[0];
  const int32_t b = speakers[1];
  const auto& sa = stats[static_cast<std::size_t>(a)];
  const auto& sb = stats[static_cast<std::size_t>(b)];
  const float a_share =
      static_cast<float>(sa.speech_us) / static_cast<float>(total_speech_us);
  const float b_share =
      static_cast<float>(sb.speech_us) / static_cast<float>(total_speech_us);
  const float combined_share = a_share + b_share;
  if (a_share < kDominantStitchMinTrackSpeechShare ||
      b_share < kDominantStitchMinTrackSpeechShare ||
      combined_share < kDominantStitchCombinedSpeechShare) {
    return;
  }

  std::int64_t overlap_us = 0;
  for (const auto& left : segments) {
    if (left.speaker_id != a) continue;
    for (const auto& right : segments) {
      if (right.speaker_id != b) continue;
      const float overlap_sec =
          std::min(left.end_sec, right.end_sec) -
          std::max(left.start_sec, right.start_sec);
      if (overlap_sec > 0.0f) {
        overlap_us += static_cast<std::int64_t>(overlap_sec * 1000000.0f);
      }
    }
  }
  const float overlap_share =
      static_cast<float>(overlap_us) /
      static_cast<float>(std::min(sa.speech_us, sb.speech_us));
  if (overlap_share > kDominantStitchMaxOverlapShare) return;

  const int32_t keep = std::min(a, b);
  const int32_t merge = std::max(a, b);
  for (auto& seg : segments) {
    if (seg.speaker_id == merge) {
      seg.speaker_id = keep;
    } else if (seg.speaker_id > merge) {
      --seg.speaker_id;
    }
  }
  --final_speaker_count;
  svp::core::trace_memory_event("diarization.dominant_track_stitch.merge", {
      {"keep_speaker", std::to_string(keep)},
      {"merged_speaker", std::to_string(merge)},
      {"combined_share", std::to_string(combined_share)},
      {"overlap_share", std::to_string(overlap_share)},
      {"final_speaker_count", std::to_string(final_speaker_count)}
  });
}

void collapse_fragmented_secondary_tracks(
    std::vector<SherpaDiarizationSegment>& segments,
    int32_t& final_speaker_count,
    std::size_t observation_count,
    const std::vector<std::vector<float>>& final_speaker_embeddings) {
  if (final_speaker_count < kFragmentedSecondaryMinFinalSpeakers ||
      observation_count < kFragmentedSecondaryMinObservations ||
      segments.empty()) {
    return;
  }

  std::vector<SpeakerTrackStats> stats(static_cast<std::size_t>(final_speaker_count));
  std::int64_t total_speech_us = 0;
  for (const auto& seg : segments) {
    if (seg.speaker_id < 0 || seg.speaker_id >= final_speaker_count) continue;
    auto& st = stats[static_cast<std::size_t>(seg.speaker_id)];
    const std::int64_t dur_us = static_cast<std::int64_t>(
        std::max(0.0f, seg.end_sec - seg.start_sec) * 1000000.0f);
    st.speech_us += dur_us;
    total_speech_us += dur_us;
    if (!st.seen) {
      st.first_start_sec = seg.start_sec;
      st.last_end_sec = seg.end_sec;
      st.seen = true;
    } else {
      st.first_start_sec = std::min(st.first_start_sec, seg.start_sec);
      st.last_end_sec = std::max(st.last_end_sec, seg.end_sec);
    }
  }
  if (total_speech_us <= 0) return;

  int32_t dominant_speaker = -1;
  int32_t largest_minority_speaker = -1;
  int32_t second_largest_minority_speaker = -1;
  std::int64_t dominant_speech_us = 0;
  std::int64_t largest_minority_speech_us = 0;
  std::int64_t second_largest_minority_speech_us = 0;
  std::int64_t minority_speech_us = 0;
  float earliest_minority_start_sec = 0.0f;
  bool saw_minority = false;
  for (int32_t sid = 0; sid < final_speaker_count; ++sid) {
    const auto& st = stats[static_cast<std::size_t>(sid)];
    if (!st.seen) continue;
    if (st.speech_us > dominant_speech_us) {
      dominant_speaker = sid;
      dominant_speech_us = st.speech_us;
    }
  }
  if (dominant_speaker < 0) return;

  for (int32_t sid = 0; sid < final_speaker_count; ++sid) {
    const auto& st = stats[static_cast<std::size_t>(sid)];
    if (!st.seen || sid == dominant_speaker) continue;
    minority_speech_us += st.speech_us;
    if (!saw_minority) {
      earliest_minority_start_sec = st.first_start_sec;
      saw_minority = true;
    } else {
      earliest_minority_start_sec =
          std::min(earliest_minority_start_sec, st.first_start_sec);
    }
    if (st.speech_us > largest_minority_speech_us) {
      second_largest_minority_speaker = largest_minority_speaker;
      second_largest_minority_speech_us = largest_minority_speech_us;
      largest_minority_speaker = sid;
      largest_minority_speech_us = st.speech_us;
    } else if (st.speech_us > second_largest_minority_speech_us) {
      second_largest_minority_speaker = sid;
      second_largest_minority_speech_us = st.speech_us;
    }
  }
  if (!saw_minority || largest_minority_speaker < 0) return;

  const float dominant_share =
      static_cast<float>(dominant_speech_us) / static_cast<float>(total_speech_us);
  const float minority_share =
      static_cast<float>(minority_speech_us) / static_cast<float>(total_speech_us);
  const float largest_minority_share =
      static_cast<float>(largest_minority_speech_us) /
      static_cast<float>(total_speech_us);
  if (dominant_share < kFragmentedSecondaryDominantMinShare ||
      dominant_share > kFragmentedSecondaryDominantMaxShare ||
      minority_share < kFragmentedSecondaryMinMinorityShare ||
      largest_minority_share > kFragmentedSecondaryMaxSingleMinorityShare) {
    return;
  }

  if (second_largest_minority_speaker < 0 ||
      static_cast<std::size_t>(largest_minority_speaker) >=
          final_speaker_embeddings.size() ||
      static_cast<std::size_t>(second_largest_minority_speaker) >=
          final_speaker_embeddings.size() ||
      !has_embedding_signal(
          final_speaker_embeddings[largest_minority_speaker]) ||
      !has_embedding_signal(
          final_speaker_embeddings[second_largest_minority_speaker])) {
    return;
  }
  const float strongest_minority_similarity = cosine_similarity(
      final_speaker_embeddings[largest_minority_speaker],
      final_speaker_embeddings[second_largest_minority_speaker]);
  if (strongest_minority_similarity <
      kFragmentedSecondaryStrongVoiceSimilarity) {
    svp::core::trace_memory_event(
        "diarization.fragmented_secondary_collapse.rejected", {
            {"reason", "strongest_minority_tracks_disagree"},
            {"largest_minority_speaker",
             std::to_string(largest_minority_speaker)},
            {"second_largest_minority_speaker",
             std::to_string(second_largest_minority_speaker)},
            {"strongest_minority_similarity",
             std::to_string(strongest_minority_similarity)}
        });
    return;
  }

  const bool dominant_starts_first =
      stats[static_cast<std::size_t>(dominant_speaker)].first_start_sec <=
      earliest_minority_start_sec;
  const int32_t dominant_final = dominant_starts_first ? 0 : 1;
  const int32_t minority_final = dominant_starts_first ? 1 : 0;
  for (auto& seg : segments) {
    if (seg.speaker_id == dominant_speaker) {
      seg.speaker_id = dominant_final;
    } else if (seg.speaker_id >= 0 && seg.speaker_id < final_speaker_count) {
      seg.speaker_id = minority_final;
    }
  }
  final_speaker_count = 2;
  svp::core::trace_memory_event("diarization.fragmented_secondary_collapse", {
      {"dominant_speaker", std::to_string(dominant_speaker)},
      {"largest_minority_speaker", std::to_string(largest_minority_speaker)},
      {"dominant_share", std::to_string(dominant_share)},
      {"minority_share", std::to_string(minority_share)},
      {"largest_minority_share", std::to_string(largest_minority_share)},
      {"strongest_minority_similarity",
       std::to_string(strongest_minority_similarity)},
      {"final_speaker_count", std::to_string(final_speaker_count)}
  });
}

void collapse_single_dominant_track(std::vector<SherpaDiarizationSegment>& segments,
                                    int32_t& final_speaker_count) {
  if (final_speaker_count <= 1 || segments.empty()) return;

  std::vector<SpeakerTrackStats> stats(static_cast<std::size_t>(final_speaker_count));
  std::int64_t total_speech_us = 0;
  for (const auto& seg : segments) {
    if (seg.speaker_id < 0 || seg.speaker_id >= final_speaker_count) continue;
    auto& st = stats[static_cast<std::size_t>(seg.speaker_id)];
    const std::int64_t dur_us = static_cast<std::int64_t>(
        std::max(0.0f, seg.end_sec - seg.start_sec) * 1000000.0f);
    st.speech_us += dur_us;
    total_speech_us += dur_us;
    st.seen = true;
  }
  if (total_speech_us <= 0) return;

  int32_t dominant_speaker = -1;
  std::int64_t dominant_speech_us = 0;
  for (int32_t sid = 0; sid < final_speaker_count; ++sid) {
    const auto& st = stats[static_cast<std::size_t>(sid)];
    if (st.seen && st.speech_us > dominant_speech_us) {
      dominant_speaker = sid;
      dominant_speech_us = st.speech_us;
    }
  }
  if (dominant_speaker < 0) return;

  const float dominant_share =
      static_cast<float>(dominant_speech_us) / static_cast<float>(total_speech_us);
  if (dominant_share < kSingleDominantCollapseSpeechShare) return;

  for (auto& seg : segments) {
    seg.speaker_id = 0;
  }
  final_speaker_count = 1;
  svp::core::trace_memory_event("diarization.single_dominant_collapse", {
      {"dominant_speaker", std::to_string(dominant_speaker)},
      {"dominant_share", std::to_string(dominant_share)},
      {"final_speaker_count", std::to_string(final_speaker_count)}
  });
}


}  // namespace svp::audio::sherpa_diarization_internal

namespace svp::audio {

using sherpa_diarization_internal::kSameSpeakerSimilarityThreshold;

ReconciliationResult reconcile_clusters(
    const std::vector<std::vector<float>>& similarity_matrix,
    const std::vector<int32_t>& cluster_ids) {

  ReconciliationResult result;
  int32_t num_clusters = static_cast<int32_t>(similarity_matrix.size());

  if (num_clusters <= 1) {
    result.cluster_to_final[0] = 0;
    result.final_speaker_count = 1;
    result.method_description = "single cluster; no merging needed";
    return result;
  }

  // Collect pairwise similarities (upper triangle)
  struct PairSim { int32_t a; int32_t b; float sim; };
  std::vector<PairSim> pairs;
  for (int32_t i = 0; i < num_clusters; ++i) {
    for (int32_t j = i + 1; j < num_clusters; ++j) {
      pairs.push_back({i, j, similarity_matrix[i][j]});
    }
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const PairSim& p1, const PairSim& p2) { return p1.sim > p2.sim; });

  // Minimum gap required to consider clusters as different speakers.
  // Below this, the embedding evidence is ambiguous — all clusters merge.
  const float kMinGap = 0.25f;

  // Fixed similarity threshold for the single-pair case.
  // Below this, two clusters are clearly different speakers.
  // Above this, they are likely the same speaker and should merge.
  float merge_threshold = 0.0f;
  float largest_gap = 0.0f;
  int32_t gap_index = -1;

  if (pairs.size() == 1) {
    // Single pair: use a fixed similarity threshold to decide merge vs split.
    // If similarity >= 0.5, merge (same speaker).
    // If similarity < 0.5, keep separate (different speakers).
    if (pairs[0].sim >= kSameSpeakerSimilarityThreshold) {
      merge_threshold = pairs[0].sim;  // will merge
      gap_index = 0;
    } else {
      merge_threshold = 2.0f;  // impossible to reach — won't merge
      gap_index = -1;
    }
    largest_gap = 1.0f - pairs[0].sim;
  } else {
    for (std::size_t i = 0; i + 1 < pairs.size(); ++i) {
      float gap = pairs[i].sim - pairs[i + 1].sim;
      if (gap > largest_gap) {
        largest_gap = gap;
        gap_index = static_cast<int32_t>(i);
        merge_threshold = pairs[i].sim;
      }
    }
  }

  // If the largest gap is too small, there's no clear voice separation.
  // Merge all clusters into one speaker.
  if (largest_gap < kMinGap && pairs.size() > 1) {
    merge_threshold = -2.0f;  // merge everything
    gap_index = -2;  // signal that min-gap override was used
  }

  // Union-find for transitive merging
  std::vector<int32_t> parent(num_clusters);
  std::iota(parent.begin(), parent.end(), 0);
  auto find = [&](int32_t x) -> int32_t {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  };
  auto unite = [&](int32_t x, int32_t y) {
    int32_t px = find(x), py = find(y);
    if (px != py) parent[px] = py;
  };

  for (const auto& p : pairs) {
    bool will_merge;
    if (gap_index == -2) {
      will_merge = true;
    } else if (gap_index >= 0) {
      will_merge = (p.sim >= merge_threshold);
    } else {
      will_merge = false;
    }
    if (will_merge) unite(p.a, p.b);
    result.merge_decisions.push_back({cluster_ids[p.a], cluster_ids[p.b], p.sim, will_merge});
  }

  // Assign final speaker IDs
  std::map<int32_t, int32_t> root_to_final;
  int32_t next_final_id = 0;
  for (std::size_t i = 0; i < cluster_ids.size(); ++i) {
    int32_t root = find(static_cast<int32_t>(i));
    if (root_to_final.find(root) == root_to_final.end()) {
      root_to_final[root] = next_final_id++;
    }
    result.cluster_to_final[cluster_ids[i]] = root_to_final[root];
  }
  result.final_speaker_count = next_final_id;

  // Build method description
  std::string desc = "largest_gap_separation: sorted_similarities=[";
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (i > 0) desc += ",";
    desc += std::to_string(pairs[i].sim);
  }
  desc += "], gap_index=" + std::to_string(gap_index);
  desc += ", merge_threshold=" + std::to_string(merge_threshold);
  desc += ", largest_gap=" + std::to_string(largest_gap);
  desc += ", preliminary_clusters=" + std::to_string(num_clusters);
  desc += ", final_speakers=" + std::to_string(next_final_id);
  result.method_description = std::move(desc);

  return result;
}


}  // namespace svp::audio
