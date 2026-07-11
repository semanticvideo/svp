#include "audio_test_support.hpp"
#include "../src/sherpa_diarization/word_assignment/types.hpp"

void test_punctuated_segment_repair_preserves_supported_group_decision() {
  using namespace svp::audio::sherpa_diarization_internal::word_assignment;
  svp::audio::AsrWord word;
  word.text = "word.";
  word.start_us = 0;
  word.end_us = 500000;
  svp::audio::SherpaDiarizationResult diarization;
  diarization.final_speaker_count = 2;
  diarization.segments = {{0.0f, 0.5f, 0}};
  AssignmentState state;
  state.assignments = {"speaker_0002"};
  state.group_decision_supported = {true};

  repair_punctuated_segment_islands({word}, diarization, state);

  assert(state.assignments[0] == "speaker_0002");
}
