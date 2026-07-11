#include "types.hpp"

#include "svp/core/memory_diagnostics.hpp"

#include <algorithm>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

void refine_word_local_assignments(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state) {
  mark_word_local_refinement_boundaries(words, diar_result, state);

  std::size_t word_local_embedding_requests = 0;
  for (std::size_t word_index = 0; word_index < words.size(); ++word_index) {
    if (!state.word_local_required[word_index]) continue;
    if (state.group_decision_supported[word_index]) continue;
    const std::int64_t start_us =
        std::max<std::int64_t>(0, words[word_index].start_us -
                                      kFingerprintWordWindowPaddingUs);
    const std::int64_t end_us =
        words[word_index].end_us + kFingerprintWordWindowPaddingUs;
    std::vector<float> word_embedding =
        compute_embedding_for_sample_range(
            api, extractor, embedding_dim, samples, start_us, end_us);
    if (!has_embedding_signal(word_embedding)) continue;

    int32_t best_speaker = -1;
    float best_similarity = -2.0f;
    float second_similarity = -2.0f;
    std::vector<float> similarities(
        static_cast<std::size_t>(diar_result.final_speaker_count), -2.0f);
    for (int32_t speaker = 0; speaker < diar_result.final_speaker_count; ++speaker) {
      const float similarity = cosine_similarity(
          word_embedding,
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

    if (best_speaker < 0 ||
        best_similarity < kFingerprintWordLocalMinSimilarity ||
        best_similarity - second_similarity < kFingerprintWordLocalMinMargin) {
      continue;
    }
    state.assignments[word_index] = speaker_id_for_index(best_speaker);
    state.word_embedding_similarities[word_index] = similarities;
    ++word_local_embedding_requests;
  }

  svp::core::trace_memory_event("diarization.asr_word_fingerprint.selective_word_local", {
      {"word_count", std::to_string(words.size())},
      {"word_local_embeddings", std::to_string(word_local_embedding_requests)}
  });
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
