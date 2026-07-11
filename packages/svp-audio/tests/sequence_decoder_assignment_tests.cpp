#include "audio_test_support.hpp"
#include "../src/sherpa_diarization/word_assignment/types.hpp"

void test_sequence_decoder_preserves_supported_group_decision() {
  using namespace svp::audio::sherpa_diarization_internal::word_assignment;
  std::vector<svp::audio::AsrWord> words;
  for (int index = 0; index < 5; ++index) {
    svp::audio::AsrWord word;
    word.text = "word";
    word.start_us = index * 200000;
    word.end_us = word.start_us + 150000;
    words.push_back(word);
  }
  svp::audio::SherpaDiarizationResult diarization;
  diarization.final_speaker_count = 2;
  AssignmentState state;
  state.assignments = {
      "speaker_0002", "speaker_0001", "speaker_0001",
      "speaker_0001", "speaker_0001"};
  state.word_embedding_similarities = {
      {1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f},
      {1.0f, 0.0f}, {1.0f, 0.0f}};
  state.group_decision_supported = {true, false, false, false, false};

  apply_sequence_decoder_assignments(words, diarization, state);

  assert(state.assignments[0] == "speaker_0002");
  assert(state.assignments[2] == "speaker_0002");
}
