#include "audio_test_support.hpp"

void test_word_serialization_uses_canonical_time_strings() {
  svp::audio::TranscriptWord word;
  word.id = "word_000001";
  word.text = "camera";
  word.normalized_text = "camera";
  word.timing = {84120000, 84490000};
  word.speaker_id = "speaker_0001";
  word.speech_region_id = "speech_000042";
  word.confidence = 0.87;
  word.speech_overlap = true;
  word.speaker_candidates = {
      {"speaker_0001", 0.72},
      {"speaker_0002", 0.41},
  };

  const nlohmann::json encoded = svp::audio::transcript_word_to_json(word);
  assert(encoded["start_us"] == 84120000);
  assert(encoded["end_us"] == 84490000);
  assert(encoded["start_sec"] == "84.120");
  assert(encoded["end_sec"] == "84.490");
  assert(encoded["speaker_id"] == "speaker_0001");
  assert(encoded["speech_overlap"] == true);
  assert(encoded["speaker_candidates"].size() == 2);
}

void test_zero_duration_span_is_rejected_for_core_span_records() {
  svp::audio::SpeechRegion region;
  region.id = "speech_000001";
  region.timing = {1000, 1000};

  bool threw = false;
  try {
    (void)svp::audio::speech_region_to_json(region);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
}

void test_attached_punctuation_can_have_zero_duration() {
  svp::audio::TranscriptWord word;
  word.id = "word_000002";
  word.text = ".";
  word.normalized_text = ".";
  word.timing = {84490000, 84490000};
  word.speaker_id = "speaker_0001";
  word.speech_region_id = "speech_000042";
  word.attached_to_word_id = "word_000001";

  const nlohmann::json encoded = svp::audio::transcript_word_to_json(word);
  assert(encoded["start_us"] == 84490000);
  assert(encoded["end_us"] == 84490000);
  assert(encoded["attached_to_word_id"] == "word_000001");
}
