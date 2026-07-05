#include "types.hpp"

namespace svp::audio::sherpa_diarization_internal::word_assignment {

void repair_interior_speaker_islands(
    const std::vector<AsrWord>& words,
    AssignmentState& state) {
  std::size_t run_start = 0;
  while (run_start < state.assignments.size()) {
    std::size_t run_end = run_start;
    while (run_end + 1 < state.assignments.size() &&
           state.assignments[run_end + 1] == state.assignments[run_start]) {
      ++run_end;
    }

    const std::size_t run_words = run_end - run_start + 1;
    const bool fluent_after =
        run_end + 1 < words.size() &&
        words[run_end + 1].start_us - words[run_end].end_us <=
            kUtteranceGapThresholdUs;
    if (run_start > 0 &&
        run_end + 1 < state.assignments.size() &&
        run_words <= kFingerprintMaxInteriorIslandWords &&
        fluent_after &&
        state.assignments[run_start - 1] == state.assignments[run_end + 1] &&
        state.assignments[run_start] != state.assignments[run_start - 1]) {
      for (std::size_t i = run_start; i <= run_end; ++i) {
        state.assignments[i] = state.assignments[run_start - 1];
      }
    }

    run_start = run_end + 1;
  }
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
