#include "audio_test_support.hpp"
#include "../src/sherpa_diarization/word_assignment/types.hpp"

void test_strong_segment_lock_preserves_supported_group_decision() {
  using namespace svp::audio::sherpa_diarization_internal::word_assignment;
  svp::audio::AsrWord word;
  AssignmentState state;
  state.assignments = {"speaker_0002"};
  state.strong_segment_assignments = {0};
  state.group_decision_supported = {true};

  apply_strong_segment_locks({word}, state);

  assert(state.assignments[0] == "speaker_0002");
}
