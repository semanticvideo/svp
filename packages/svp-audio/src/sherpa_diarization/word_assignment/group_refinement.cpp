#include "types.hpp"

namespace svp::audio::sherpa_diarization_internal::word_assignment {
namespace {

bool group_has_non_dominant_local_evidence(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    const AssignmentState& state,
    std::size_t first_word,
    std::size_t last_word) {
  for (std::size_t i = first_word; i <= last_word && i < state.assignments.size(); ++i) {
    const int32_t speaker =
        speaker_index_from_id(state.assignments[i],
                              diar_result.final_speaker_count);
    if (speaker >= 0 && speaker != state.dominant_speaker) return true;
  }
  return false;
}

}  // namespace

void refine_groups_by_embedding(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state) {
  auto assign_group_or_subgroups = [&](std::size_t first_word,
                                       std::size_t last_word) {
    if (first_word > last_word) return;
    const bool has_local_evidence =
        group_has_non_dominant_local_evidence(
            words, diar_result, state, first_word, last_word);
    const std::int64_t duration_us =
        words[last_word].end_us - words[first_word].start_us;
    const std::size_t word_count = last_word - first_word + 1;
    if (has_local_evidence ||
        (word_count <= kFingerprintSubUtteranceMaxWords &&
         duration_us <= kFingerprintSubUtteranceMaxDurationUs)) {
      assign_group_by_embedding(
          api, extractor, embedding_dim, samples, words, diar_result, state,
          first_word, last_word);
      return;
    }

    if (diar_result.final_speaker_count > 2) {
      assign_group_by_embedding(
          api, extractor, embedding_dim, samples, words, diar_result, state,
          first_word, last_word);
      return;
    }

    std::size_t sub_start = first_word;
    while (sub_start <= last_word) {
      std::size_t sub_end = sub_start;
      while (sub_end < last_word &&
             sub_end - sub_start + 1 < kFingerprintSubUtteranceMaxWords &&
             words[sub_end + 1].end_us - words[sub_start].start_us <=
                 kFingerprintSubUtteranceMaxDurationUs) {
        ++sub_end;
      }
      assign_group_by_embedding(
          api, extractor, embedding_dim, samples, words, diar_result, state,
          sub_start, sub_end);
      sub_start = sub_end + 1;
    }
  };

  std::size_t group_start = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const bool last_word = i + 1 == words.size();
    const bool gap_after =
        !last_word &&
        words[i + 1].start_us - words[i].end_us > kUtteranceGapThresholdUs;
    if (last_word || gap_after || ends_utterance(words[i].text)) {
      assign_group_or_subgroups(group_start, i);
      group_start = i + 1;
    }
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
