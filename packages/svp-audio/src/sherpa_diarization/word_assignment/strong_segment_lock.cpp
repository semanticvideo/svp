#include "types.hpp"

#include "svp/core/memory_diagnostics.hpp"

namespace svp::audio::sherpa_diarization_internal::word_assignment {

void apply_strong_segment_locks(
    const std::vector<AsrWord>& words,
    AssignmentState& state) {
  std::size_t strong_segment_locks_applied = 0;
  for (std::size_t i = 0; i < state.assignments.size(); ++i) {
    if (state.strong_segment_assignments[i] < 0) continue;
    const std::string segment_speaker_id =
        speaker_id_for_index(state.strong_segment_assignments[i]);
    if (state.assignments[i] != segment_speaker_id) {
      state.assignments[i] = segment_speaker_id;
      ++strong_segment_locks_applied;
    }
  }
  if (strong_segment_locks_applied > 0) {
    svp::core::trace_memory_event("diarization.asr_word_fingerprint.segment_lock", {
        {"word_count", std::to_string(words.size())},
        {"locks_applied", std::to_string(strong_segment_locks_applied)}
    });
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
