#include "svp/audio/audio_extraction_plan.hpp"

#include <nlohmann/json.hpp>

#include <iomanip>
#include <sstream>

namespace svp::audio {
namespace {

std::string stream_output_ref(std::size_t ordinal) {
  std::ostringstream output;
  output << "media/audio/original_stream_" << std::setw(3) << std::setfill('0')
         << ordinal << ".flac";
  return output.str();
}

std::string stream_task_id(std::size_t ordinal) {
  std::ostringstream output;
  output << "task.audio.extract.astream_" << std::setw(3) << std::setfill('0')
         << ordinal;
  return output.str();
}

std::vector<std::string> original_stream_arguments(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    const svp::media::AudioStreamProbe& stream,
    const std::string& output_ref) {
  return {
      ffmpeg_path.string(),
      "-hide_banner",
      "-nostdin",
      "-y",
      "-i",
      source_path.string(),
      "-map",
      "0:" + std::to_string(stream.index),
      "-vn",
      "-c:a",
      "flac",
      output_ref,
  };
}

std::vector<std::string> analysis_audio_arguments(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    const svp::media::AudioStreamProbe& stream,
    const std::string& output_ref) {
  return {
      ffmpeg_path.string(),
      "-hide_banner",
      "-nostdin",
      "-y",
      "-i",
      source_path.string(),
      "-map",
      "0:" + std::to_string(stream.index),
      "-vn",
      "-ac",
      "1",
      "-ar",
      "16000",
      "-c:a",
      "pcm_s16le",
      output_ref,
  };
}

nlohmann::json extraction_command_to_json(const AudioExtractionCommandPlan& command) {
  return {
      {"task_id", command.task_id},
      {"source_audio_stream_id", command.source_audio_stream_id},
      {"source_stream_index", command.source_stream_index},
      {"output_ref", command.output_ref},
      {"arguments", command.arguments},
      {"command_available", !command.arguments.empty()},
  };
}

nlohmann::json audio_absence_plan_to_json(const AudioAbsenceArtifactPlan& plan) {
  return {
      {"task_id", plan.task_id},
      {"depends_on", plan.depends_on},
      {"processor_id", plan.processor_id},
      {"output_ref", plan.output_ref},
      {"staged_output_written", false},
  };
}

nlohmann::json waveform_plan_to_json(const WaveformArtifactPlan& plan) {
  return {
      {"task_id", plan.task_id},
      {"depends_on", plan.depends_on},
      {"processor_id", plan.processor_id},
      {"input_ref", plan.input_ref},
      {"output_ref", plan.output_ref},
      {"window_duration_us", plan.window_duration_us},
      {"waveform_run", false},
      {"waveform_written", false},
      {"pending_reason", "waveform envelope generation requires staged analysis audio"},
  };
}

nlohmann::json provenance_plan_to_json(const AudioProvenanceArtifactPlan& plan) {
  return {
      {"task_id", plan.task_id},
      {"depends_on", plan.depends_on},
      {"output_ref", plan.output_ref},
      {"staged_output_written", false},
      {"final_package_provenance_written", false},
  };
}

}  // namespace

AudioExtractionPlan build_audio_extraction_plan(const std::filesystem::path& source_path,
                                                const svp::media::MediaProbe& probe,
                                                bool ffmpeg_available,
                                                const std::filesystem::path& ffmpeg_path) {
  AudioExtractionPlan plan;
  plan.source_path = source_path;
  plan.ffmpeg_path = ffmpeg_path;
  plan.ffmpeg_available = ffmpeg_available;
  plan.source_audio_present = !probe.audio_streams.empty();
  std::vector<std::string> extraction_task_ids;

  for (std::size_t index = 0; index < probe.audio_streams.size(); ++index) {
    const svp::media::AudioStreamProbe& stream = probe.audio_streams[index];
    const std::string output_ref = stream_output_ref(index);
    const std::string task_id = stream_task_id(index);
    plan.original_streams.push_back(AudioExtractionCommandPlan{
        task_id,
        stream.id,
        stream.index,
        output_ref,
        ffmpeg_available ? original_stream_arguments(ffmpeg_path, source_path, stream, output_ref)
                         : std::vector<std::string>{},
    });
    extraction_task_ids.push_back(task_id);
  }

  plan.analysis_audio.output_ref = "media/audio/analysis_mono_16k.wav";
  if (probe.audio_streams.size() == 1) {
    const svp::media::AudioStreamProbe& stream = probe.audio_streams.front();
    plan.analysis_audio.task_id = "task.audio.analysis.astream_000";
    plan.analysis_audio.depends_on = {"task.audio.extract.astream_000"};
    plan.analysis_audio.selected_source_audio_stream_id = stream.id;
    plan.analysis_audio.arguments =
        ffmpeg_available
            ? analysis_audio_arguments(ffmpeg_path,
                                       source_path,
                                       stream,
                                       plan.analysis_audio.output_ref)
            : std::vector<std::string>{};
  } else if (probe.audio_streams.size() > 1) {
    plan.analysis_audio.task_id = "task.audio.analysis.pending_vad_selection";
    for (std::size_t index = 0; index < probe.audio_streams.size(); ++index) {
      plan.analysis_audio.depends_on.push_back(stream_task_id(index));
    }
    plan.analysis_audio.selected_source_audio_stream_id = "pending_vad_speech_positive_selection";
    plan.blockers.push_back(
        "analysis audio selection for multiple source streams requires VAD speech-positive stream or mix decision");
  } else {
    plan.analysis_audio.task_id = "task.audio.analysis.silence_000";
    plan.analysis_audio.selected_source_audio_stream_id = "canonical_silence";
    plan.blockers.push_back(
        "canonical silent FLAC and analysis WAV generation is not implemented in this foundation pass");
  }

  plan.audio_absence.task_id = "task.audio.absence.write";
  plan.audio_absence.depends_on = extraction_task_ids;
  plan.audio_absence.processor_id = "proc_audio_absence_foundation_0001";
  plan.audio_absence.output_ref = "media/audio/audio_absence.json";

  plan.waveform.task_id = "task.audio.waveform.analysis_mono_16k";
  if (!plan.analysis_audio.task_id.empty()) {
    plan.waveform.depends_on = {plan.analysis_audio.task_id};
  }
  plan.waveform.processor_id = "proc_waveform_envelope_0001";
  plan.waveform.input_ref = plan.analysis_audio.output_ref;
  plan.waveform.output_ref = "media/audio/waveform.jsonl";

  plan.processor_provenance.task_id = "task.audio.provenance.plan";
  plan.processor_provenance.depends_on = extraction_task_ids;
  if (!plan.analysis_audio.task_id.empty()) {
    plan.processor_provenance.depends_on.push_back(plan.analysis_audio.task_id);
  }
  plan.processor_provenance.depends_on.push_back(plan.audio_absence.task_id);
  plan.processor_provenance.depends_on.push_back(plan.waveform.task_id);
  plan.processor_provenance.output_ref = "provenance/processors.jsonl";

  if (!ffmpeg_available) {
    plan.blockers.push_back(
        "FFmpeg executable/library was not verified; extraction commands are documented but not runnable");
  }

  return plan;
}

nlohmann::json audio_extraction_plan_to_json(const AudioExtractionPlan& plan) {
  nlohmann::json original_streams = nlohmann::json::array();
  for (const AudioExtractionCommandPlan& command : plan.original_streams) {
    original_streams.push_back(extraction_command_to_json(command));
  }

  return {
      {"source_path", plan.source_path.string()},
      {"ffmpeg_path", plan.ffmpeg_path.string()},
      {"ffmpeg_available", plan.ffmpeg_available},
      {"source_audio_present", plan.source_audio_present},
      {"original_streams", original_streams},
      {"analysis_audio",
       {{"task_id", plan.analysis_audio.task_id},
        {"depends_on", plan.analysis_audio.depends_on},
        {"selected_source_audio_stream_id",
         plan.analysis_audio.selected_source_audio_stream_id},
        {"output_ref", plan.analysis_audio.output_ref},
        {"arguments", plan.analysis_audio.arguments},
        {"command_available", !plan.analysis_audio.arguments.empty()}}},
      {"audio_absence", audio_absence_plan_to_json(plan.audio_absence)},
      {"waveform", waveform_plan_to_json(plan.waveform)},
      {"processor_provenance", provenance_plan_to_json(plan.processor_provenance)},
      {"blockers", plan.blockers},
      {"extraction_run", false},
      {"analysis_audio_written", false},
  };
}

}  // namespace svp::audio
