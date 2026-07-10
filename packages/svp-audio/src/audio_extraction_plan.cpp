#include "svp/audio/audio_extraction_plan.hpp"

#include "svp/media/canonical_timing.hpp"

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

std::string microphone_analysis_output_ref(std::size_t ordinal) {
  std::ostringstream output;
  output << "media/audio/analysis_stream_" << std::setw(3) << std::setfill('0')
         << ordinal << "_mono_16k.wav";
  return output.str();
}

std::string microphone_analysis_task_id(std::size_t ordinal) {
  std::ostringstream output;
  output << "task.audio.analysis.microphone_" << std::setw(3)
         << std::setfill('0') << ordinal;
  return output.str();
}

const svp::media::StreamTiming& primary_presentation_timing(
    const svp::media::MediaProbe& probe) {
  if (!probe.video_streams.empty()) return probe.video_streams.front().timing;
  if (probe.container_timing.has_value()) return *probe.container_timing;
  const svp::media::StreamTiming* earliest = &probe.audio_streams.front().timing;
  for (const auto& stream : probe.audio_streams) {
    const __int128 left = static_cast<__int128>(stream.timing.start_pts) *
                          stream.timing.timebase.numerator *
                          earliest->timebase.denominator;
    const __int128 right = static_cast<__int128>(earliest->start_pts) *
                           earliest->timebase.numerator *
                           stream.timing.timebase.denominator;
    if (left < right) earliest = &stream.timing;
  }
  return *earliest;
}

std::int64_t normalized_stream_start_us(
    const svp::media::AudioStreamProbe& stream,
    const svp::media::StreamTiming& origin) {
  return svp::media::pts_delta_to_microseconds(
      stream.timing.start_pts, stream.timing.timebase,
      origin.start_pts, origin.timebase);
}

std::optional<std::int64_t> stream_timeline_duration_us(
    const svp::media::AudioStreamProbe& stream,
    const svp::media::StreamTiming& origin) {
  if (!stream.timing.duration_pts.has_value()) return std::nullopt;
  const std::int64_t start_us =
      normalized_stream_start_us(stream, origin);
  const std::int64_t duration_us = svp::media::pts_to_microseconds(
      *stream.timing.duration_pts, stream.timing.timebase);
  return std::max<std::int64_t>(0, start_us + duration_us);
}

std::string signed_seconds_string(std::int64_t microseconds) {
  if (microseconds >= 0) {
    return svp::media::microseconds_to_seconds_string(microseconds);
  }
  return "-" + svp::media::microseconds_to_seconds_string(-microseconds);
}

std::string timeline_normalization_filter(std::int64_t source_start_us) {
  return "asetpts=PTS-STARTPTS+" + signed_seconds_string(source_start_us) +
         "/TB,aresample=async=1:first_pts=0";
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
      "-nostats",
      "-v",
      "error",
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

std::vector<std::string> single_stream_analysis_audio_arguments(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    const svp::media::AudioStreamProbe& stream,
    const std::string& output_ref) {
  return {
      ffmpeg_path.string(), "-hide_banner", "-nostdin", "-nostats", "-v",
      "error", "-y", "-i", source_path.string(), "-map",
      "0:" + std::to_string(stream.index), "-vn", "-ac", "1", "-ar",
      "16000", "-c:a", "pcm_s16le", output_ref,
  };
}

std::vector<std::string> microphone_analysis_audio_arguments(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    const svp::media::AudioStreamProbe& stream,
    std::int64_t source_start_us,
    const std::string& output_ref) {
  return {
      ffmpeg_path.string(),
      "-hide_banner",
      "-nostdin",
      "-nostats",
      "-v",
      "error",
      "-y",
      "-i",
      source_path.string(),
      "-map",
      "0:" + std::to_string(stream.index),
      "-vn",
      "-af",
      timeline_normalization_filter(source_start_us),
      "-ac",
      "1",
      "-ar",
      "16000",
      "-c:a",
      "pcm_s16le",
      output_ref,
  };
}

std::vector<std::string> mixed_analysis_audio_arguments(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    const std::vector<svp::media::AudioStreamProbe>& streams,
    const svp::media::StreamTiming& origin,
    const std::string& output_ref) {
  std::ostringstream inputs;
  std::ostringstream normalized_inputs;
  for (std::size_t index = 0; index < streams.size(); ++index) {
    inputs << "[0:" << streams[index].index
           << "]" << timeline_normalization_filter(
                            normalized_stream_start_us(streams[index], origin))
           << "[mic" << index << "];";
    normalized_inputs << "[mic" << index << "]";
  }
  std::ostringstream filter;
  filter << inputs.str() << normalized_inputs.str()
         << "amix=inputs=" << streams.size()
         << ":duration=longest:normalize=1[mixed]";
  return {
      ffmpeg_path.string(), "-hide_banner", "-nostdin", "-nostats", "-v",
      "error", "-y", "-i", source_path.string(), "-filter_complex",
      filter.str(), "-map", "[mixed]", "-vn", "-ac", "1", "-ar", "16000",
      "-c:a", "pcm_s16le", output_ref,
  };
}

nlohmann::json analysis_command_to_json(const AnalysisAudioCommandPlan& command) {
  return {
      {"task_id", command.task_id},
      {"depends_on", command.depends_on},
      {"selected_source_audio_stream_id", command.selected_source_audio_stream_id},
      {"source_stream_index", command.source_stream_index},
      {"source_start_us", command.source_start_us},
      {"timeline_duration_us",
       command.timeline_duration_us.has_value()
           ? nlohmann::json(*command.timeline_duration_us)
           : nlohmann::json(nullptr)},
      {"output_ref", command.output_ref},
      {"arguments", command.arguments},
      {"command_available", !command.arguments.empty()},
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
  const svp::media::StreamTiming* presentation_origin =
      probe.audio_streams.empty() ? nullptr
                                  : &primary_presentation_timing(probe);

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
    plan.analysis_audio.source_stream_index = stream.index;
    plan.analysis_audio.source_start_us =
        normalized_stream_start_us(stream, *presentation_origin);
    plan.analysis_audio.timeline_duration_us =
        stream_timeline_duration_us(stream, *presentation_origin);
    plan.analysis_audio.arguments =
        ffmpeg_available
            ? single_stream_analysis_audio_arguments(
                  ffmpeg_path, source_path, stream,
                  plan.analysis_audio.output_ref)
            : std::vector<std::string>{};
  } else if (probe.audio_streams.size() > 1) {
    plan.analysis_audio.task_id = "task.audio.analysis.canonical_mix";
    plan.analysis_audio.depends_on = extraction_task_ids;
    plan.analysis_audio.selected_source_audio_stream_id = "mixed_microphone_streams";
    plan.analysis_audio.arguments =
        ffmpeg_available
            ? mixed_analysis_audio_arguments(ffmpeg_path, source_path,
                                             probe.audio_streams,
                                             *presentation_origin,
                                             plan.analysis_audio.output_ref)
            : std::vector<std::string>{};
    for (std::size_t index = 0; index < probe.audio_streams.size(); ++index) {
      const auto& stream = probe.audio_streams[index];
      AnalysisAudioCommandPlan microphone;
      microphone.task_id = microphone_analysis_task_id(index);
      microphone.depends_on = {stream_task_id(index)};
      microphone.selected_source_audio_stream_id = stream.id;
      microphone.source_stream_index = stream.index;
      microphone.source_start_us =
          normalized_stream_start_us(stream, *presentation_origin);
      microphone.timeline_duration_us =
          stream_timeline_duration_us(stream, *presentation_origin);
      microphone.output_ref = microphone_analysis_output_ref(index);
      microphone.arguments =
          ffmpeg_available
              ? microphone_analysis_audio_arguments(
                    ffmpeg_path, source_path, stream,
                    microphone.source_start_us, microphone.output_ref)
              : std::vector<std::string>{};
      plan.microphone_analysis_streams.push_back(std::move(microphone));
    }
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
  for (const auto& microphone : plan.microphone_analysis_streams) {
    plan.processor_provenance.depends_on.push_back(microphone.task_id);
  }
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

  nlohmann::json microphone_analysis_streams = nlohmann::json::array();
  for (const auto& command : plan.microphone_analysis_streams) {
    microphone_analysis_streams.push_back(analysis_command_to_json(command));
  }

  return {
      {"source_path", plan.source_path.string()},
      {"ffmpeg_path", plan.ffmpeg_path.string()},
      {"ffmpeg_available", plan.ffmpeg_available},
      {"source_audio_present", plan.source_audio_present},
      {"original_streams", original_streams},
      {"microphone_analysis_streams", microphone_analysis_streams},
      {"analysis_audio",
       {{"task_id", plan.analysis_audio.task_id},
        {"depends_on", plan.analysis_audio.depends_on},
        {"selected_source_audio_stream_id",
         plan.analysis_audio.selected_source_audio_stream_id},
        {"source_stream_index", plan.analysis_audio.source_stream_index},
        {"source_start_us", plan.analysis_audio.source_start_us},
        {"timeline_duration_us",
         plan.analysis_audio.timeline_duration_us.has_value()
             ? nlohmann::json(*plan.analysis_audio.timeline_duration_us)
             : nlohmann::json(nullptr)},
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
