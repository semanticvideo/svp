#include "types.hpp"

namespace svp::audio::sherpa_diarization_internal::word_assignment {

void apply_sequence_decoder_assignments(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state) {
  std::vector<WordSpeakerEvidence> decoder_evidence(words.size());
  for (std::size_t i = 0; i < words.size(); ++i) {
    decoder_evidence[i].segment_speaker =
        speaker_index_from_id(state.assignments[i],
                              diar_result.final_speaker_count);
    decoder_evidence[i].embedding_similarity_by_speaker =
        state.word_embedding_similarities[i];
  }

  const std::vector<int32_t> decoded =
      decode_word_speaker_sequence(
          words, diar_result.final_speaker_count, decoder_evidence);
  if (decoded.size() != state.assignments.size()) return;

  for (std::size_t i = 0; i < state.assignments.size(); ++i) {
    if (state.group_decision_supported[i]) continue;
    if (decoded[i] >= 0) {
      state.assignments[i] = speaker_id_for_index(decoded[i]);
    }
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
