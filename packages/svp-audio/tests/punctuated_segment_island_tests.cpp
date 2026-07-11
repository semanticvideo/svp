#include "audio_test_support.hpp"
#include "../src/sherpa_diarization/word_assignment/types.hpp"

void test_punctuated_segment_repair_preserves_supported_group_decision() {
  using namespace svp::audio::sherpa_diarization_internal::word_assignment;
  std::vector<svp::audio::AsrWord> words(3);
  words[0].text = "before";
  words[0].start_us = 0;
  words[0].end_us = 400000;
  words[1].text = "word.";
  words[1].start_us = 400000;
  words[1].end_us = 800000;
  words[2].text = "after";
  words[2].start_us = 800000;
  words[2].end_us = 1200000;
  svp::audio::SherpaDiarizationResult diarization;
  diarization.final_speaker_count = 2;
  diarization.segments = {
      {0.0f, 0.4f, 0}, {0.4f, 0.8f, 0}, {0.8f, 1.2f, 0}};
  AssignmentState state;
  state.assignments = {
      "speaker_0001", "speaker_0002", "speaker_0001"};
  state.group_decision_supported = {false, true, false};

  repair_punctuated_segment_islands(words, diarization, state);

  assert(state.assignments[1] == "speaker_0002");
}
