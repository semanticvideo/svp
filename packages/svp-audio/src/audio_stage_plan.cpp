#include "svp/audio/audio_stage_plan.hpp"

#include <nlohmann/json.hpp>

namespace svp::audio {
namespace {

std::vector<std::string> required_audio_outputs() {
  return {
      "media/audio/original_stream_NNN.flac",
      "media/audio/analysis_mono_16k.wav",
      "media/audio/waveform.jsonl",
      "media/audio/audio_absence.json",
      "transcript/speech_regions.jsonl",
      "transcript/transcript.json",
      "transcript/words.jsonl",
      "transcript/speakers.jsonl",
      "transcript/speaker_segments.jsonl",
      "provenance/processors.jsonl",
  };
}

}  // namespace

AudioStagePlan build_audio_stage_plan(const std::filesystem::path& source_path,
                                      const svp::media::MediaProbe& probe,
                                      bool ffmpeg_audio_extraction_available) {
  AudioStagePlan plan;
  plan.source_path = source_path;
  plan.source_audio_present = !probe.audio_streams.empty();
  if (plan.source_audio_present) {
    plan.selected_audio_stream_id = probe.audio_streams.front().id;
  }

  plan.required_outputs = required_audio_outputs();
  plan.pending_processors = {
      "ffmpeg_audio_extraction",
      "waveform_envelope",
      "silero_vad_onnx",
      "whispercpp_transcription",
      "sherpa_onnx_diarization",
      "speaker_word_assignment",
  };

  if (!ffmpeg_audio_extraction_available) {
    plan.blockers.push_back(
        "FFmpeg audio extraction executable/library was not verified for this skeleton run");
  }
  plan.blockers.push_back("VAD model runtime is not wired in this foundation pass");
  plan.blockers.push_back("whisper.cpp transcription is not wired in this foundation pass");
  plan.blockers.push_back("sherpa-onnx diarization is not wired in this foundation pass");

  return plan;
}

nlohmann::json audio_stage_plan_to_json(const AudioStagePlan& plan) {
  return {
      {"source_path", plan.source_path.string()},
      {"source_audio_present", plan.source_audio_present},
      {"selected_audio_stream_id", plan.selected_audio_stream_id},
      {"required_outputs", plan.required_outputs},
      {"pending_processors", plan.pending_processors},
      {"blockers", plan.blockers},
      {"transcription_run", false},
      {"diarization_run", false},
      {"valid_svp_package_written", false},
  };
}

}  // namespace svp::audio
