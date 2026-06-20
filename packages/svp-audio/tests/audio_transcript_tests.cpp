#include "svp/audio/audio_stage_plan.hpp"
#include "svp/audio/transcript_records.hpp"

#include <cassert>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

namespace {

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

void test_audio_stage_plan_is_honest_about_pending_processors() {
  svp::media::MediaProbe probe;
  probe.audio_streams.push_back({"astream_0001", 1, "aac", 48000, 2, {}});

  const svp::audio::AudioStagePlan plan =
      svp::audio::build_audio_stage_plan("sample.mov", probe, false);
  const nlohmann::json encoded = svp::audio::audio_stage_plan_to_json(plan);

  assert(encoded["source_audio_present"] == true);
  assert(encoded["selected_audio_stream_id"] == "astream_0001");
  assert(encoded["transcription_run"] == false);
  assert(encoded["diarization_run"] == false);
  assert(encoded["valid_svp_package_written"] == false);
  assert(!encoded["blockers"].empty());

  const std::vector<std::string> required_outputs =
      encoded["required_outputs"].get<std::vector<std::string>>();
  assert(std::find(required_outputs.begin(),
                   required_outputs.end(),
                   "media/audio/waveform.jsonl") != required_outputs.end());
  assert(std::find(required_outputs.begin(),
                   required_outputs.end(),
                   "media/audio/audio_absence.json") != required_outputs.end());
  assert(std::find(required_outputs.begin(), required_outputs.end(), "audio/waveform.jsonl") ==
         required_outputs.end());
  assert(std::find(required_outputs.begin(), required_outputs.end(), "audio/audio_absence.json") ==
         required_outputs.end());
}

}  // namespace

int main() {
  test_word_serialization_uses_canonical_time_strings();
  test_zero_duration_span_is_rejected_for_core_span_records();
  test_attached_punctuation_can_have_zero_duration();
  test_audio_stage_plan_is_honest_about_pending_processors();
  return 0;
}
