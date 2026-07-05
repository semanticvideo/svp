#include "types.hpp"

#include "svp/core/memory_diagnostics.hpp"

#include <algorithm>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

void assign_group_by_embedding(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state,
    std::size_t first_word,
    std::size_t last_word) {
  if (first_word > last_word || last_word >= words.size()) return;
  const std::int64_t start_us =
      std::max<std::int64_t>(0, words[first_word].start_us -
                                    kUtteranceEmbeddingPaddingUs);
  const std::int64_t end_us =
      words[last_word].end_us + kUtteranceEmbeddingPaddingUs;
  std::vector<float> group_embedding =
      compute_embedding_for_sample_range(
          api, extractor, embedding_dim, samples, start_us, end_us);
  if (!has_embedding_signal(group_embedding)) return;

  int32_t best_speaker = -1;
  float best_similarity = -2.0f;
  float second_similarity = -2.0f;
  std::vector<float> similarities(
      static_cast<std::size_t>(diar_result.final_speaker_count), -2.0f);
  for (int32_t speaker = 0; speaker < diar_result.final_speaker_count; ++speaker) {
    const float similarity = cosine_similarity(
        group_embedding,
        state.fingerprints[static_cast<std::size_t>(speaker)].prototype);
    similarities[static_cast<std::size_t>(speaker)] = similarity;
    if (similarity > best_similarity) {
      second_similarity = best_similarity;
      best_similarity = similarity;
      best_speaker = speaker;
    } else if (similarity > second_similarity) {
      second_similarity = similarity;
    }
  }

  std::vector<std::size_t> local_counts(
      static_cast<std::size_t>(diar_result.final_speaker_count), 0);
  for (std::size_t i = first_word; i <= last_word; ++i) {
    const int32_t speaker =
        speaker_index_from_id(state.assignments[i],
                              diar_result.final_speaker_count);
    if (speaker >= 0) ++local_counts[static_cast<std::size_t>(speaker)];
  }

  int32_t local_evidence_speaker = -1;
  std::size_t local_evidence_count = 0;
  for (int32_t speaker = 0; speaker < diar_result.final_speaker_count; ++speaker) {
    if (speaker == state.dominant_speaker) continue;
    const std::size_t count = local_counts[static_cast<std::size_t>(speaker)];
    if (count > local_evidence_count) {
      local_evidence_speaker = speaker;
      local_evidence_count = count;
    }
  }

  const float margin = best_similarity - second_similarity;
  int32_t selected_speaker = -1;
  if (local_evidence_speaker >= 0 &&
      local_evidence_count >= kFingerprintLocalEvidenceMinWords &&
      similarities[static_cast<std::size_t>(local_evidence_speaker)] >=
          kUtteranceEmbeddingMinSimilarity &&
      best_similarity -
          similarities[static_cast<std::size_t>(local_evidence_speaker)] <=
          kFingerprintLocalEvidenceMaxContraryMargin) {
    selected_speaker = local_evidence_speaker;
  } else if (best_speaker >= 0 &&
             best_similarity >= kUtteranceEmbeddingMinSimilarity &&
             margin >= kUtteranceEmbeddingMinMargin) {
    selected_speaker = best_speaker;
  }

  svp::core::trace_memory_event("diarization.asr_word_fingerprint.group", {
      {"first_word", std::to_string(first_word)},
      {"last_word", std::to_string(last_word)},
      {"best_speaker", std::to_string(best_speaker)},
      {"selected_speaker", std::to_string(selected_speaker)},
      {"best_similarity", std::to_string(best_similarity)},
      {"second_similarity", std::to_string(second_similarity)},
      {"margin", std::to_string(margin)},
      {"local_evidence_speaker", std::to_string(local_evidence_speaker)},
      {"local_evidence_count", std::to_string(local_evidence_count)}
  });

  if (selected_speaker < 0) return;

  const std::string speaker_id = speaker_id_for_index(selected_speaker);
  for (std::size_t i = first_word; i <= last_word; ++i) {
    state.assignments[i] = speaker_id;
  }
  if (margin < kSelectiveWordLocalUnstableGroupMargin) {
    for (std::size_t i = first_word; i <= last_word; ++i) {
      state.word_local_required[i] = true;
    }
  }

  if (margin >= kFingerprintUpdateMinMargin) {
    update_fingerprint(
        state.fingerprints[static_cast<std::size_t>(selected_speaker)],
        group_embedding);
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
