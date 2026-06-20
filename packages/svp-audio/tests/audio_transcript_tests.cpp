#include "svp/audio/audio_stage_plan.hpp"
#include "svp/audio/transcript_records.hpp"
#include "svp/audio/vad_task_plan.hpp"

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
  assert(encoded["audio_extraction"]["ffmpeg_available"] == false);
  assert(encoded["audio_extraction"]["extraction_run"] == false);
  assert(encoded["vad_task_plan"]["vad_run"] == false);
  assert(encoded["vad_task_plan"]["speech_regions_written"] == false);

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

void test_audio_extraction_plan_documents_ffmpeg_commands_when_available() {
  svp::media::MediaProbe probe;
  probe.audio_streams.push_back({"astream_0001", 1, "aac", 48000, 2, {}});

  const svp::audio::AudioStagePlan plan =
      svp::audio::build_audio_stage_plan("sample.mov", probe, true);
  const nlohmann::json extraction =
      svp::audio::audio_stage_plan_to_json(plan)["audio_extraction"];

  assert(extraction["ffmpeg_available"] == true);
  assert(extraction["ffmpeg_path"] == "ffmpeg");
  assert(extraction["original_streams"].size() == 1);
  assert(extraction["original_streams"][0]["task_id"] == "task.audio.extract.astream_000");
  assert(extraction["original_streams"][0]["output_ref"] ==
         "media/audio/original_stream_000.flac");
  assert(extraction["original_streams"][0]["arguments"][0] == "ffmpeg");
  const std::vector<std::string> original_arguments =
      extraction["original_streams"][0]["arguments"].get<std::vector<std::string>>();
  assert(std::find(original_arguments.begin(), original_arguments.end(), "0:1") !=
         original_arguments.end());
  assert(extraction["analysis_audio"]["task_id"] == "task.audio.analysis.astream_000");
  assert(extraction["analysis_audio"]["selected_source_audio_stream_id"] == "astream_0001");
  assert(extraction["analysis_audio"]["output_ref"] == "media/audio/analysis_mono_16k.wav");
  assert(extraction["analysis_audio"]["command_available"] == true);
  assert(extraction["extraction_run"] == false);
  assert(extraction["analysis_audio_written"] == false);
}

void test_multi_stream_analysis_audio_waits_for_vad_selection() {
  svp::media::MediaProbe probe;
  probe.audio_streams.push_back({"astream_0001", 1, "aac", 48000, 2, {}});
  probe.audio_streams.push_back({"astream_0002", 2, "aac", 48000, 2, {}});

  const svp::audio::AudioStagePlan plan =
      svp::audio::build_audio_stage_plan("sample.mov", probe, true);
  const nlohmann::json extraction =
      svp::audio::audio_stage_plan_to_json(plan)["audio_extraction"];

  assert(extraction["original_streams"].size() == 2);
  assert(extraction["analysis_audio"]["task_id"] ==
         "task.audio.analysis.pending_vad_selection");
  assert(extraction["analysis_audio"]["selected_source_audio_stream_id"] ==
         "pending_vad_speech_positive_selection");
  assert(extraction["analysis_audio"]["depends_on"].size() == 2);
  assert(extraction["analysis_audio"]["arguments"].empty());
  assert(extraction["analysis_audio"]["command_available"] == false);
  assert(!extraction["blockers"].empty());
}

void test_vad_task_plan_uses_stable_thirty_second_boundaries() {
  svp::media::MediaProbe probe;
  probe.audio_streams.push_back({"astream_0001", 1, "aac", 48000, 2, {}});

  const svp::audio::VadTaskPlan plan =
      svp::audio::build_vad_task_plan(probe, 65000000, false);
  const nlohmann::json encoded = svp::audio::vad_task_plan_to_json(plan);

  assert(encoded["runtime"] == "onnxruntime");
  assert(encoded["model_id"] == "model_silero_vad");
  assert(encoded["chunk_duration_us"] == 30000000);
  assert(encoded["tasks"].size() == 3);
  assert(encoded["tasks"][0]["task_id"] == "task.vad.audio_chunk_000000");
  assert(encoded["tasks"][0]["start_us"] == 0);
  assert(encoded["tasks"][0]["end_us"] == 30000000);
  assert(encoded["tasks"][2]["start_us"] == 60000000);
  assert(encoded["tasks"][2]["end_us"] == 65000000);
  assert(encoded["tasks"][2]["input_refs"][0] == "media/audio/analysis_mono_16k.wav");
  assert(encoded["tasks"][2]["output_refs"][0] ==
         "transcript/speech_regions.chunk_000002.jsonl");
  assert(!encoded["blockers"].empty());
  assert(encoded["vad_run"] == false);
}

}  // namespace

int main() {
  test_word_serialization_uses_canonical_time_strings();
  test_zero_duration_span_is_rejected_for_core_span_records();
  test_attached_punctuation_can_have_zero_duration();
  test_audio_stage_plan_is_honest_about_pending_processors();
  test_audio_extraction_plan_documents_ffmpeg_commands_when_available();
  test_multi_stream_analysis_audio_waits_for_vad_selection();
  test_vad_task_plan_uses_stable_thirty_second_boundaries();
  return 0;
}
