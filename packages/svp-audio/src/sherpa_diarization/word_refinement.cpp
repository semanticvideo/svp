#include "private.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>

namespace svp::audio::sherpa_diarization_internal {

bool ends_utterance(const std::string& text) {
  if (text.empty()) return false;
  const char last = text.back();
  return last == '.' || last == '?' || last == '!';
}


}  // namespace svp::audio::sherpa_diarization_internal

namespace svp::audio {

using namespace sherpa_diarization_internal;

std::vector<std::string> refine_word_speakers_by_embedding(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result) {

  // Return empty on failure paths so the caller falls back to
  // segment-overlap assignment instead of overriding with speaker_unknown.
  if (words.empty() || diar_result.cluster_centroids.empty()) {
    return {};
  }

  const SherpaDiarizationApi& api = get_api();
  if (!api.lib_handle || !api.emb_create) {
    return {};
  }

  const std::filesystem::path embedding_model =
      model_dir / "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";
  if (!std::filesystem::exists(embedding_model)) {
    return {};
  }

  std::vector<float> samples;
  try {
    samples = read_pcm_s16le_mono_wav_samples(wav_path);
  } catch (...) {
    return {};
  }
  if (samples.empty()) return {};

  const std::string emb_path = embedding_model.string();
  SherpaOnnxSpeakerEmbeddingExtractorConfig emb_config;
  std::memset(&emb_config, 0, sizeof(emb_config));
  emb_config.model = emb_path.c_str();
  emb_config.num_threads = 1;
  emb_config.debug = 0;
  emb_config.provider = "cpu";

  const void* extractor = api.emb_create(&emb_config);
  if (!extractor) return {};

  const int32_t embedding_dim = api.emb_dim(extractor);

  if (diar_result.cluster_centroids.empty()) {
    api.emb_destroy(extractor);
    return {};
  }

  // Compute embeddings for each diarization segment (full segment, no splitting).
  struct SubSeg {
    float start_sec;
    float end_sec;
    std::vector<float> embedding;
  };
  std::vector<SubSeg> sub_segs;

  auto compute_embedding_for_range = [&](int32_t start_sample,
                                          int32_t num_samples) -> std::vector<float> {
    if (num_samples < 1600) return {};
    const SherpaOnnxOnlineStream* stream = api.emb_create_stream(extractor);
    if (!stream) return {};
    api.stream_accept(stream, 16000, samples.data() + start_sample, num_samples);
    api.stream_input_finished(stream);
    std::vector<float> emb;
    if (api.emb_is_ready(extractor, stream)) {
      const float* result = api.emb_compute(extractor, stream);
      if (result) {
        emb.assign(result, result + embedding_dim);
        api.emb_destroy_vec(result);
      }
    }
    api.stream_destroy(stream);
    if (!emb.empty()) normalize_embedding(emb);
    return emb;
  };

  const std::vector<SherpaDiarizationSegment>& diar_segments =
      !diar_result.preliminary_segments.empty()
          ? diar_result.preliminary_segments
          : diar_result.segments;

  for (const auto& seg : diar_segments) {
    int32_t start_sample = static_cast<int32_t>(seg.start_sec * 16000.0f);
    int32_t end_sample = static_cast<int32_t>(seg.end_sec * 16000.0f);
    if (start_sample < 0) start_sample = 0;
    if (end_sample > static_cast<int32_t>(samples.size()))
      end_sample = static_cast<int32_t>(samples.size());

    std::vector<float> emb = compute_embedding_for_range(
        start_sample, end_sample - start_sample);
    if (!emb.empty()) {
      sub_segs.push_back({seg.start_sec, seg.end_sec, std::move(emb)});
    }
  }

  api.emb_destroy(extractor);

  if (sub_segs.empty()) return {};

  // Agglomerative clustering with largest-gap separation.
  // Merge clusters bottom-up, tracking the similarity at each merge.
  // Stop when we find the largest gap in merge similarities — this is
  // the natural point where two distinct speakers separate.
  std::size_t n = sub_segs.size();
  std::vector<int32_t> cluster_id(n);
  std::iota(cluster_id.begin(), cluster_id.end(), 0);

  // Compute pairwise similarities
  std::vector<std::vector<float>> sim(n, std::vector<float>(n, 0.0f));
  for (std::size_t i = 0; i < n; ++i) {
    sim[i][i] = 1.0f;
    for (std::size_t j = i + 1; j < n; ++j) {
      float s = cosine_similarity(sub_segs[i].embedding, sub_segs[j].embedding);
      sim[i][j] = s;
      sim[j][i] = s;
    }
  }

  // Cluster centroids for agglomerative merging
  std::map<int32_t, std::vector<std::size_t>> cluster_members;
  std::map<int32_t, std::vector<float>> cluster_centroids;
  for (std::size_t i = 0; i < n; ++i) {
    cluster_members[cluster_id[i]] = {i};
    cluster_centroids[cluster_id[i]] = sub_segs[i].embedding;
  }

  auto cluster_similarity = [&](int32_t c1, int32_t c2) -> float {
    float total = 0.0f;
    std::size_t count = 0;
    for (std::size_t m1 : cluster_members[c1]) {
      for (std::size_t m2 : cluster_members[c2]) {
        total += sim[m1][m2];
        ++count;
      }
    }
    return count > 0 ? total / count : -2.0f;
  };

  // Track merge similarities to find the largest gap
  std::vector<float> merge_sims;

  while (cluster_members.size() > 1) {
    float best_sim = -2.0f;
    int32_t best_c1 = -1, best_c2 = -1;
    for (auto it1 = cluster_members.begin(); it1 != cluster_members.end(); ++it1) {
      auto it2 = it1;
      ++it2;
      for (; it2 != cluster_members.end(); ++it2) {
        float s = cluster_similarity(it1->first, it2->first);
        if (s > best_sim) {
          best_sim = s;
          best_c1 = it1->first;
          best_c2 = it2->first;
        }
      }
    }
    if (best_c1 < 0) break;

    merge_sims.push_back(best_sim);

    // Merge c2 into c1
    for (std::size_t m : cluster_members[best_c2]) {
      cluster_id[m] = best_c1;
      cluster_members[best_c1].push_back(m);
    }
    auto& centroid = cluster_centroids[best_c1];
    std::fill(centroid.begin(), centroid.end(), 0.0f);
    for (std::size_t m : cluster_members[best_c1]) {
      for (int d = 0; d < embedding_dim; ++d) {
        centroid[d] += sub_segs[m].embedding[d];
      }
    }
    for (int d = 0; d < embedding_dim; ++d) {
      centroid[d] /= static_cast<float>(cluster_members[best_c1].size());
    }
    cluster_members.erase(best_c2);
    cluster_centroids.erase(best_c2);
  }

  // Find the largest gap in merge similarities to determine the cut point.
  // merge_sims is in descending order (most similar merge first). The largest
  // gap between consecutive merge sims indicates where intra-speaker merges
  // end and inter-speaker merges begin.
  // num_merges = how many of the top merges to replay (intra-speaker only).
  // We exclude the very last gap (after the second-to-last merge) because the
  // final merge always has an artificially large gap — it merges the last
  // two remaining clusters, which are naturally the most dissimilar.
  std::size_t num_merges = 0;  // default: don't merge anything (all separate)
  if (merge_sims.size() > 1) {
    float largest_gap = 0.0f;
    std::size_t gap_idx = 0;
    // Consider gaps 0..(merge_sims.size()-2), skipping only the last gap
    std::size_t num_gaps = merge_sims.size() - 1;
    // When there are 3+ merges, skip the last gap (it's artificial).
    // With 2 merges (1 gap), still consider it.
    std::size_t gaps_to_check = (num_gaps > 1) ? num_gaps - 1 : num_gaps;
    for (std::size_t i = 0; i < gaps_to_check; ++i) {
      float gap = merge_sims[i] - merge_sims[i + 1];
      if (gap > largest_gap) {
        largest_gap = gap;
        gap_idx = i;
      }
    }
    // Merges 0..gap_idx (inclusive) are intra-speaker
    num_merges = gap_idx + 1;
    // If the largest gap is too small, treat all as one speaker
    if (largest_gap < 0.10f) {
      num_merges = merge_sims.size();  // merge everything
    }
  }

  // Re-run clustering but only perform num_merges intra-speaker merges
  std::iota(cluster_id.begin(), cluster_id.end(), 0);
  cluster_members.clear();
  cluster_centroids.clear();
  for (std::size_t i = 0; i < n; ++i) {
    cluster_members[cluster_id[i]] = {i};
    cluster_centroids[cluster_id[i]] = sub_segs[i].embedding;
  }

  for (std::size_t step = 0; step < num_merges && cluster_members.size() > 1; ++step) {
    float best_sim = -2.0f;
    int32_t best_c1 = -1, best_c2 = -1;
    for (auto it1 = cluster_members.begin(); it1 != cluster_members.end(); ++it1) {
      auto it2 = it1;
      ++it2;
      for (; it2 != cluster_members.end(); ++it2) {
        float s = cluster_similarity(it1->first, it2->first);
        if (s > best_sim) {
          best_sim = s;
          best_c1 = it1->first;
          best_c2 = it2->first;
        }
      }
    }
    if (best_c1 < 0) break;

    for (std::size_t m : cluster_members[best_c2]) {
      cluster_id[m] = best_c1;
      cluster_members[best_c1].push_back(m);
    }
    cluster_members.erase(best_c2);
    cluster_centroids.erase(best_c2);
  }

  // Map cluster IDs to sequential speaker IDs (0, 1)
  // Order by first appearance in time
  std::map<int32_t, int32_t> cluster_to_speaker;
  int32_t next_speaker = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (cluster_to_speaker.find(cluster_id[i]) == cluster_to_speaker.end()) {
      cluster_to_speaker[cluster_id[i]] = next_speaker++;
    }
  }

  // Assign words to speakers based on sub-segments using max overlap
  std::vector<std::string> assignments(words.size(), "speaker_unknown");
  for (std::size_t i = 0; i < words.size(); ++i) {
    std::int64_t w_start = words[i].start_us;
    std::int64_t w_end = words[i].end_us;
    int32_t best_speaker = -1;
    std::int64_t best_overlap = 0;
    for (std::size_t j = 0; j < sub_segs.size(); ++j) {
      std::int64_t s_start = static_cast<std::int64_t>(sub_segs[j].start_sec * 1000000.0f);
      std::int64_t s_end = static_cast<std::int64_t>(sub_segs[j].end_sec * 1000000.0f);
      std::int64_t overlap = std::min(w_end, s_end) - std::max(w_start, s_start);
      if (overlap > best_overlap) {
        best_overlap = overlap;
        best_speaker = cluster_to_speaker[cluster_id[j]];
      }
    }
    if (best_speaker < 0) {
      std::int64_t nearest_dist = std::numeric_limits<std::int64_t>::max();
      for (std::size_t j = 0; j < sub_segs.size(); ++j) {
        std::int64_t s_start = static_cast<std::int64_t>(sub_segs[j].start_sec * 1000000.0f);
        std::int64_t s_end = static_cast<std::int64_t>(sub_segs[j].end_sec * 1000000.0f);
        std::int64_t dist;
        if (w_end <= s_start) dist = s_start - w_end;
        else if (w_start >= s_end) dist = w_start - s_end;
        else dist = 0;
        if (dist < nearest_dist) {
          nearest_dist = dist;
          best_speaker = cluster_to_speaker[cluster_id[j]];
        }
      }
      if (nearest_dist > 500000) best_speaker = -1;
    }
    if (best_speaker >= 0) {
      std::ostringstream sid;
      sid << "speaker_" << std::setw(4) << std::setfill('0') << (best_speaker + 1);
      assignments[i] = sid.str();
    }
  }

  const bool has_unknown_assignment =
      std::find(assignments.begin(), assignments.end(), "speaker_unknown") !=
      assignments.end();

  float max_pairwise_similarity = -2.0f;
  for (std::size_t i = 0; i < diar_result.pairwise_similarity_matrix.size(); ++i) {
    for (std::size_t j = i + 1; j < diar_result.pairwise_similarity_matrix[i].size(); ++j) {
      max_pairwise_similarity =
          std::max(max_pairwise_similarity, diar_result.pairwise_similarity_matrix[i][j]);
    }
  }
  const bool likely_overmerged_single_speaker =
      diar_result.final_speaker_count == 1 &&
      max_pairwise_similarity > -2.0f &&
      max_pairwise_similarity < kSameSpeakerSimilarityThreshold;

  if (has_unknown_assignment &&
      (diar_result.final_speaker_count > 1 || likely_overmerged_single_speaker)) {
    struct UtteranceGroup {
      std::size_t first_word = 0;
      std::size_t last_word = 0;
    };

    std::vector<UtteranceGroup> groups;
    std::size_t group_start = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
      const bool last_word = i + 1 == words.size();
      const bool gap_after =
          !last_word &&
          words[i + 1].start_us - words[i].end_us > kUtteranceGapThresholdUs;
      if (last_word || gap_after || ends_utterance(words[i].text)) {
        groups.push_back({group_start, i});
        group_start = i + 1;
      }
    }

    if (groups.size() >= 2) {
      std::vector<std::string> group_assignments(words.size(), "");
      for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        std::ostringstream sid;
        sid << "speaker_" << std::setw(4) << std::setfill('0')
            << (group_index + 1);
        for (std::size_t word_index = groups[group_index].first_word;
             word_index <= groups[group_index].last_word &&
             word_index < group_assignments.size();
             ++word_index) {
          group_assignments[word_index] = sid.str();
        }
      }

      const bool group_has_unknown =
          std::any_of(group_assignments.begin(), group_assignments.end(),
                      [](const std::string& sid) { return sid.empty(); });
      if (!group_has_unknown) {
        return group_assignments;
      }
    }

    auto nearest_known_assignment = [&](std::size_t word_index,
                                        std::size_t first_word,
                                        std::size_t last_word) -> std::string {
      std::string best;
      std::size_t best_distance = std::numeric_limits<std::size_t>::max();
      for (std::size_t i = first_word; i <= last_word && i < assignments.size(); ++i) {
        if (assignments[i] == "speaker_unknown") continue;
        const std::size_t distance =
            (i > word_index) ? (i - word_index) : (word_index - i);
        if (distance < best_distance) {
          best_distance = distance;
          best = assignments[i];
        }
      }
      return best;
    };

    for (const UtteranceGroup& group : groups) {
      for (std::size_t i = group.first_word;
           i <= group.last_word && i < assignments.size();
           ++i) {
        if (assignments[i] != "speaker_unknown") continue;
        const std::string speaker =
            nearest_known_assignment(i, group.first_word, group.last_word);
        if (!speaker.empty()) {
          assignments[i] = speaker;
        }
      }
    }

    for (std::size_t i = 0; i < assignments.size(); ++i) {
      if (assignments[i] != "speaker_unknown") continue;
      const std::string speaker =
          nearest_known_assignment(i, 0, assignments.empty() ? 0 : assignments.size() - 1);
      if (!speaker.empty()) {
        assignments[i] = speaker;
      }
    }
  }

  if (has_unknown_assignment &&
      diar_result.final_speaker_count == 1 &&
      !likely_overmerged_single_speaker) {
    for (std::string& assignment : assignments) {
      if (assignment == "speaker_unknown") {
        assignment = "speaker_0001";
      }
    }
  }

  return assignments;
}

}  // namespace svp::audio
