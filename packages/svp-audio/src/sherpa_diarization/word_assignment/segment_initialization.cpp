#include "types.hpp"

namespace svp::audio::sherpa_diarization_internal::word_assignment {

void initialize_segment_assignments(
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result,
    AssignmentState& state) {
  state.strong_segment_assignments.assign(words.size(), -1);
  state.assignments.assign(words.size(), "speaker_unknown");
  state.word_embedding_similarities.assign(
      words.size(),
      std::vector<float>(
          static_cast<std::size_t>(diar_result.final_speaker_count), -2.0f));
  state.word_local_required.assign(words.size(), false);
  state.group_decision_supported.assign(words.size(), false);

  for (std::size_t i = 0; i < words.size(); ++i) {
    const int32_t segment_speaker =
        best_segment_speaker_for_word(words[i], diar_result.segments);
    const SegmentOverlap overlap =
        best_segment_overlap_for_word(words[i], diar_result.segments);
    if (diar_result.final_speaker_count > 2 &&
        overlap.speaker >= 0 &&
        overlap.overlap_fraction >= kMultiSpeakerStrongSegmentOverlapLock) {
      state.strong_segment_assignments[i] = overlap.speaker;
    }
    if (segment_speaker >= 0) {
      state.assignments[i] = speaker_id_for_index(segment_speaker);
    } else {
      state.word_local_required[i] = true;
    }
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
