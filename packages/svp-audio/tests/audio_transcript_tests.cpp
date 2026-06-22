#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/audio_extraction_executor.hpp"
#include "svp/audio/audio_stage_plan.hpp"
#include "svp/audio/diarization_boundary.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/transcript_records.hpp"
#include "svp/audio/transcript_writer.hpp"
#include "svp/audio/vad_execution_boundary.hpp"
#include "svp/audio/vad_task_plan.hpp"
#include "svp/audio/waveform_envelope.hpp"
#include "svp/audio/whisper_mel.hpp"
#include "svp/audio/whisper_model.hpp"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void write_u16_le(std::ostream& output, std::uint16_t value) {
  output.put(static_cast<char>(value & 0xff));
  output.put(static_cast<char>((value >> 8) & 0xff));
}

void write_u32_le(std::ostream& output, std::uint32_t value) {
  output.put(static_cast<char>(value & 0xff));
  output.put(static_cast<char>((value >> 8) & 0xff));
  output.put(static_cast<char>((value >> 16) & 0xff));
  output.put(static_cast<char>((value >> 24) & 0xff));
}

void write_pcm_s16le_mono_wav(const std::filesystem::path& path,
                              const std::vector<std::int16_t>& samples) {
  std::ofstream output(path, std::ios::binary);
  const std::uint32_t data_size = static_cast<std::uint32_t>(samples.size() * 2);
  output.write("RIFF", 4);
  write_u32_le(output, 36 + data_size);
  output.write("WAVE", 4);
  output.write("fmt ", 4);
  write_u32_le(output, 16);
  write_u16_le(output, 1);
  write_u16_le(output, 1);
  write_u32_le(output, 16000);
  write_u32_le(output, 16000 * 2);
  write_u16_le(output, 2);
  write_u16_le(output, 16);
  output.write("data", 4);
  write_u32_le(output, data_size);
  for (const std::int16_t sample : samples) {
    write_u16_le(output, static_cast<std::uint16_t>(sample));
  }
}

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
  assert(encoded["vad_execution_boundary"]["vad_run"] == false);
  assert(encoded["vad_execution_boundary"]["speech_regions_written"] == false);
  assert(encoded["vad_execution_boundary"]["final_output_ref"] ==
         "transcript/speech_regions.jsonl");
  assert(encoded["vad_execution_boundary"]["analysis_audio_available"] == false);
  assert(encoded["vad_execution_boundary"]["waveform_available"] == false);
  assert(encoded["vad_execution_boundary"]["model_runtime_available"] == false);

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
  assert(extraction["audio_absence"]["output_ref"] == "media/audio/audio_absence.json");
  assert(extraction["audio_absence"]["processor_id"] ==
         "proc_audio_absence_foundation_0001");
  assert(extraction["waveform"]["output_ref"] == "media/audio/waveform.jsonl");
  assert(extraction["waveform"]["input_ref"] == "media/audio/analysis_mono_16k.wav");
  assert(extraction["waveform"]["window_duration_us"] == 10000);
  assert(extraction["waveform"]["waveform_run"] == false);
  assert(extraction["waveform"]["waveform_written"] == false);
  assert(extraction["waveform"]["pending_reason"] ==
         "waveform envelope generation requires staged analysis audio");
  assert(extraction["processor_provenance"]["output_ref"] ==
         "provenance/processors.jsonl");
  assert(extraction["processor_provenance"]["final_package_provenance_written"] ==
         false);
  assert(extraction["extraction_run"] == false);
  assert(extraction["analysis_audio_written"] == false);
}

void test_multi_stream_analysis_audio_selects_first_stream() {
  svp::media::MediaProbe probe;
  probe.audio_streams.push_back({"astream_0001", 1, "aac", 48000, 2, {}});
  probe.audio_streams.push_back({"astream_0002", 2, "aac", 48000, 2, {}});

  const svp::audio::AudioStagePlan plan =
      svp::audio::build_audio_stage_plan("sample.mov", probe, true);
  const nlohmann::json extraction =
      svp::audio::audio_stage_plan_to_json(plan)["audio_extraction"];

  assert(extraction["original_streams"].size() == 2);
  assert(extraction["analysis_audio"]["task_id"] == "task.audio.analysis.astream_000");
  assert(extraction["analysis_audio"]["selected_source_audio_stream_id"] == "astream_0001");
  assert(extraction["analysis_audio"]["depends_on"].size() == 1);
  assert(extraction["analysis_audio"]["command_available"] == true);
  assert(!extraction["analysis_audio"]["arguments"].empty());
  assert(!extraction["blockers"].empty());
  bool has_multi_stream_blocker = false;
  for (const auto& blocker : extraction["blockers"]) {
    if (blocker.get<std::string>().find("multiple audio streams") != std::string::npos) {
      has_multi_stream_blocker = true;
      break;
    }
  }
  assert(has_multi_stream_blocker);
}

void test_waveform_envelope_generates_ten_millisecond_json_records() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-audio-waveform-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const std::filesystem::path wav_path = root / "analysis_mono_16k.wav";
  write_pcm_s16le_mono_wav(wav_path, std::vector<std::int16_t>(320, 16384));

  const std::vector<svp::audio::WaveformEnvelopeRecord> records =
      svp::audio::generate_waveform_envelope_records(wav_path, 10000);
  assert(records.size() == 2);
  const nlohmann::json first =
      svp::audio::waveform_envelope_record_to_json(records.front());
  assert(first["id"] == "wave_00000000");
  assert(first["start_us"] == 0);
  assert(first["end_us"] == 10000);
  assert(first["rms_db"] == -6.0);
  assert(first["peak_db"] == -6.0);
  const nlohmann::json second =
      svp::audio::waveform_envelope_record_to_json(records.back());
  assert(second["id"] == "wave_00000001");
  assert(second["start_us"] == 10000);
  assert(second["end_us"] == 20000);

  std::filesystem::remove_all(root);
}

void test_audio_extraction_executor_writes_staged_single_stream_outputs() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-audio-executor-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "audio-source.txt";
  {
    std::ofstream output(source);
    output << "fake audio bytes\n";
  }
  const std::filesystem::path analysis_source = root / "analysis-source.wav";
  write_pcm_s16le_mono_wav(analysis_source, std::vector<std::int16_t>(320, 16384));

  svp::audio::AudioExtractionPlan plan;
  plan.source_path = source;
  plan.ffmpeg_available = true;
  plan.source_audio_present = true;
  plan.original_streams.push_back(svp::audio::AudioExtractionCommandPlan{
      "task.audio.extract.astream_000",
      "astream_0001",
      1,
      "media/audio/original_stream_000.flac",
      {"/bin/cp", source.string(), "media/audio/original_stream_000.flac"},
  });
  plan.analysis_audio.task_id = "task.audio.analysis.astream_000";
  plan.analysis_audio.depends_on = {"task.audio.extract.astream_000"};
  plan.analysis_audio.selected_source_audio_stream_id = "astream_0001";
  plan.analysis_audio.output_ref = "media/audio/analysis_mono_16k.wav";
  plan.analysis_audio.arguments = {
      "/bin/cp",
      analysis_source.string(),
      "media/audio/analysis_mono_16k.wav",
  };
  plan.audio_absence.task_id = "task.audio.absence.write";
  plan.audio_absence.depends_on = {"task.audio.extract.astream_000"};
  plan.audio_absence.processor_id = "proc_audio_absence_foundation_0001";
  plan.audio_absence.output_ref = "media/audio/audio_absence.json";
  plan.waveform.task_id = "task.audio.waveform.analysis_mono_16k";
  plan.waveform.depends_on = {"task.audio.analysis.astream_000"};
  plan.waveform.processor_id = "proc_waveform_envelope_0001";
  plan.waveform.input_ref = "media/audio/analysis_mono_16k.wav";
  plan.waveform.output_ref = "media/audio/waveform.jsonl";
  plan.processor_provenance.task_id = "task.audio.provenance.plan";
  plan.processor_provenance.output_ref = "provenance/processors.jsonl";

  const std::filesystem::path staging_root = root / "staging";
  const svp::audio::AudioExtractionRun run =
      svp::audio::execute_audio_extraction_plan(plan, staging_root);
  const nlohmann::json encoded = svp::audio::audio_extraction_run_to_json(run);

  assert(encoded["extraction_run"] == true);
  assert(encoded["original_streams_written"] == true);
  assert(encoded["analysis_audio_written"] == true);
  assert(encoded["audio_absence_written"] == true);
  assert(encoded["waveform_written"] == true);
  assert(encoded["processor_provenance_written"] == true);
  assert(encoded["original_streams"][0]["command_executed"] == true);
  assert(encoded["analysis_audio"]["command_executed"] == true);
  assert(encoded["audio_absence"]["written"] == true);
  assert(encoded["waveform"]["written"] == true);
  assert(encoded["processor_provenance"]["written"] == true);
  assert(std::filesystem::exists(staging_root / "media/audio/original_stream_000.flac"));
  assert(std::filesystem::exists(staging_root / "media/audio/analysis_mono_16k.wav"));
  assert(std::filesystem::exists(staging_root / "media/audio/audio_absence.json"));
  assert(std::filesystem::exists(staging_root / "media/audio/waveform.jsonl"));
  assert(std::filesystem::exists(staging_root / "provenance/processors.jsonl"));

  {
    std::ifstream input(staging_root / "media/audio/audio_absence.json");
    const nlohmann::json absence = nlohmann::json::parse(input);
    assert(absence["source_audio_present"] == true);
    assert(absence["source_audio_stream_count"] == 1);
    assert(absence["selected_source_audio_stream_id"] == "astream_0001");
    assert(absence["analysis_audio_written"] == true);
    assert(absence["final_package_ready"] == false);
  }

  {
    std::ifstream input(staging_root / "media/audio/waveform.jsonl");
    std::string line;
    std::getline(input, line);
    const nlohmann::json first = nlohmann::json::parse(line);
    assert(first["id"] == "wave_00000000");
    assert(first["start_us"] == 0);
    assert(first["end_us"] == 10000);
    assert(first["rms_db"] == -6.0);
    assert(first["peak_db"] == -6.0);
    std::getline(input, line);
    const nlohmann::json second = nlohmann::json::parse(line);
    assert(second["id"] == "wave_00000001");
    assert(!std::getline(input, line));
  }

  {
    std::ifstream input(staging_root / "provenance/processors.jsonl");
    std::string line;
    std::getline(input, line);
    std::getline(input, line);
    std::getline(input, line);
    const nlohmann::json waveform_processor = nlohmann::json::parse(line);
    assert(waveform_processor["id"] == "proc_waveform_envelope_0001");
    assert(waveform_processor["runtime"] == "svp-audio");
    assert(waveform_processor["foundation_status"] == "staged");
    assert(waveform_processor["completed"] == true);
  }

  std::filesystem::remove_all(root);
}

void test_audio_extraction_executor_leaves_multi_stream_analysis_unrun() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-audio-executor-blocked-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "audio-source.txt";
  {
    std::ofstream output(source);
    output << "fake audio bytes\n";
  }

  svp::audio::AudioExtractionPlan plan;
  plan.source_audio_present = true;
  plan.original_streams.push_back(svp::audio::AudioExtractionCommandPlan{
      "task.audio.extract.astream_000",
      "astream_0001",
      1,
      "media/audio/original_stream_000.flac",
      {"/bin/cp", source.string(), "media/audio/original_stream_000.flac"},
  });
  plan.original_streams.push_back(svp::audio::AudioExtractionCommandPlan{
      "task.audio.extract.astream_001",
      "astream_0002",
      2,
      "media/audio/original_stream_001.flac",
      {"/bin/cp", source.string(), "media/audio/original_stream_001.flac"},
  });
  plan.analysis_audio.task_id = "task.audio.analysis.pending_vad_selection";
  plan.analysis_audio.output_ref = "media/audio/analysis_mono_16k.wav";
  plan.analysis_audio.selected_source_audio_stream_id =
      "pending_vad_speech_positive_selection";
  plan.audio_absence.task_id = "task.audio.absence.write";
  plan.audio_absence.depends_on = {"task.audio.extract.astream_000",
                                   "task.audio.extract.astream_001"};
  plan.audio_absence.processor_id = "proc_audio_absence_foundation_0001";
  plan.audio_absence.output_ref = "media/audio/audio_absence.json";
  plan.waveform.task_id = "task.audio.waveform.analysis_mono_16k";
  plan.waveform.depends_on = {"task.audio.analysis.pending_vad_selection"};
  plan.waveform.processor_id = "proc_waveform_envelope_0001";
  plan.waveform.input_ref = "media/audio/analysis_mono_16k.wav";
  plan.waveform.output_ref = "media/audio/waveform.jsonl";
  plan.processor_provenance.task_id = "task.audio.provenance.plan";
  plan.processor_provenance.output_ref = "provenance/processors.jsonl";

  const std::filesystem::path staging_root = root / "staging";
  const svp::audio::AudioExtractionRun run =
      svp::audio::execute_audio_extraction_plan(plan, staging_root);
  const nlohmann::json encoded = svp::audio::audio_extraction_run_to_json(run);

  assert(encoded["extraction_run"] == true);
  assert(encoded["original_streams_written"] == true);
  assert(encoded["analysis_audio_written"] == false);
  assert(encoded["audio_absence_written"] == true);
  assert(encoded["waveform_written"] == false);
  assert(encoded["processor_provenance_written"] == true);
  assert(encoded["analysis_audio"]["command_available"] == false);
  assert(encoded["analysis_audio"]["command_executed"] == false);
  assert(encoded["audio_absence"]["written"] == true);
  assert(encoded["waveform"]["written"] == false);
  assert(!encoded["blockers"].empty());

  std::filesystem::remove_all(root);
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

void test_vad_execution_boundary_preserves_honest_unrun_state() {
  svp::media::MediaProbe probe;
  probe.audio_streams.push_back({"astream_0001", 1, "aac", 48000, 2, {}});

  const svp::audio::VadTaskPlan plan =
      svp::audio::build_vad_task_plan(probe, 65000000, false);
  const svp::audio::VadExecutionBoundary boundary =
      svp::audio::build_vad_execution_boundary(plan, true, true, false);
  const nlohmann::json encoded = svp::audio::vad_execution_boundary_to_json(boundary);

  assert(encoded["processor_id"] == "proc_silero_vad_0001");
  assert(encoded["model_id"] == "model_silero_vad");
  assert(encoded["runtime"] == "onnxruntime");
  assert(encoded["analysis_audio_available"] == true);
  assert(encoded["waveform_available"] == true);
  assert(encoded["model_runtime_available"] == false);
  assert(encoded["vad_run"] == false);
  assert(encoded["speech_regions_written"] == false);
  assert(encoded["speech_region_count"] == 0);
  assert(encoded["final_output_ref"] == "transcript/speech_regions.jsonl");
  assert(encoded["task_ids"].size() == 3);
  assert(encoded["staged_chunk_output_refs"].size() == 3);
  assert(encoded["staged_chunk_output_refs"][2] ==
         "transcript/speech_regions.chunk_000002.jsonl");
  assert(!encoded["blockers"].empty());
}

void test_execute_vad_boundary_handles_runtime_unavailable_honestly() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-audio-vad-exec-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::media::MediaProbe probe;
  probe.audio_streams.push_back({"astream_0001", 1, "aac", 48000, 2, {}});

  const svp::audio::VadTaskPlan plan =
      svp::audio::build_vad_task_plan(probe, 65000000, false);

  // Case 1: model_runtime_available is false
  {
    const svp::audio::VadExecutionBoundary boundary =
        svp::audio::build_vad_execution_boundary(plan, true, true, false);
    const svp::audio::VadExecutionBoundary result =
        svp::audio::execute_vad_boundary(boundary, root);
    assert(result.vad_run == false);
    assert(result.speech_regions_written == false);
    assert(result.speech_region_count == 0);
  }

  // Case 2: model_runtime_available is true (throws runtime_unavailable)
  {
    const svp::audio::VadExecutionBoundary boundary =
        svp::audio::build_vad_execution_boundary(plan, true, true, true);
    const svp::audio::VadExecutionBoundary result =
        svp::audio::execute_vad_boundary(boundary, root);
    assert(result.vad_run == false);
    assert(result.speech_regions_written == false);
    assert(result.speech_region_count == 0);
    assert(!result.blockers.empty());
    bool found_error = false;
    for (const auto& blocker : result.blockers) {
      if (blocker.find("runtime_unavailable") != std::string::npos ||
          blocker.find("support is not configured") != std::string::npos ||
          blocker.find("missing_model") != std::string::npos ||
          blocker.find("No ONNX model file") != std::string::npos) {
        found_error = true;
      }
    }
    assert(found_error);
  }

  std::filesystem::remove_all(root);
}

void test_asr_chunk_plan_produces_correct_overlapping_chunks() {
  const svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(65000000, 30000000, 5000000);
  const nlohmann::json encoded = svp::audio::asr_chunk_plan_to_json(plan);

  assert(encoded["chunk_count"] == 3);
  assert(encoded["chunk_duration_us"] == 30000000);
  assert(encoded["overlap_us"] == 5000000);
  assert(encoded["total_duration_us"] == 65000000);

  assert(plan.chunks.size() == 3);
  assert(plan.chunks[0].source_start_us == 0);
  assert(plan.chunks[0].source_end_us == 30000000);
  assert(plan.chunks[0].overlap_before_us == 0);
  assert(plan.chunks[0].overlap_after_us == 5000000);
  assert(plan.chunks[0].chunk_id == "asr_chunk_000000");
  assert(plan.chunks[0].output_ref == "transcript/words.chunk_000000.jsonl");

  assert(plan.chunks[1].source_start_us == 25000000);
  assert(plan.chunks[1].source_end_us == 55000000);
  assert(plan.chunks[1].overlap_before_us == 5000000);
  assert(plan.chunks[1].overlap_after_us == 5000000);

  assert(plan.chunks[2].source_start_us == 50000000);
  assert(plan.chunks[2].source_end_us == 65000000);
  assert(plan.chunks[2].overlap_before_us == 5000000);
  assert(plan.chunks[2].overlap_after_us == 0);
}

void test_asr_chunk_plan_zero_duration_produces_no_chunks() {
  const svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(0, 30000000, 5000000);
  assert(plan.chunks.empty());
}

void test_asr_chunk_plan_rejects_bad_parameters() {
  bool threw = false;
  try {
    (void)svp::audio::build_asr_chunk_plan(-1, 30000000, 5000000);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);

  threw = false;
  try {
    (void)svp::audio::build_asr_chunk_plan(1000000, 0, 5000000);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);

  threw = false;
  try {
    (void)svp::audio::build_asr_chunk_plan(1000000, 30000000, 30000000);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
}

void test_asr_chunk_plan_exact_multiple_has_no_trailing_chunk() {
  const svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(60000000, 30000000, 0);
  assert(plan.chunks.size() == 2);
  assert(plan.chunks[0].source_start_us == 0);
  assert(plan.chunks[0].source_end_us == 30000000);
  assert(plan.chunks[1].source_start_us == 30000000);
  assert(plan.chunks[1].source_end_us == 60000000);
  assert(plan.chunks[1].overlap_before_us == 0);
  assert(plan.chunks[1].overlap_after_us == 0);
}

void test_overlap_reconciliation_deduplicates_boundary_words() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);
  assert(plan.chunks.size() == 2);
  assert(plan.chunks[0].source_start_us == 0);
  assert(plan.chunks[0].source_end_us == 20000000);
  assert(plan.chunks[1].source_start_us == 15000000);
  assert(plan.chunks[1].source_end_us == 30000000);
  assert(plan.chunks[1].overlap_before_us == 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);

  chunk_words[0].push_back({"hello", 1000000, 1500000, 0.9, 0});
  chunk_words[0].push_back({"world", 16000000, 16500000, 0.9, 0});
  chunk_words[0].push_back({"overlap_word", 17000000, 17500000, 0.85, 0});

  chunk_words[1].push_back({"overlap_word", 17000000 - 15000000, 17500000 - 15000000, 0.85, 1});
  chunk_words[1].push_back({"final", 25000000 - 15000000, 26000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 4);
  assert(reconciled[0].text == "hello");
  assert(reconciled[0].start_us == 1000000);
  assert(reconciled[1].text == "world");
  assert(reconciled[1].start_us == 16000000);
  assert(reconciled[2].text == "overlap_word");
  assert(reconciled[2].start_us == 17000000);
  assert(reconciled[3].text == "final");
  assert(reconciled[3].start_us == 25000000);
}

void test_overlap_reconciliation_duplicate_in_actual_overlap_region() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);
  assert(plan.chunks[1].source_start_us == 15000000);
  assert(plan.chunks[1].overlap_before_us == 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  chunk_words[0].push_back({"alpha", 16000000, 16500000, 0.9, 0});
  chunk_words[1].push_back({"alpha", 16000000 - 15000000, 16500000 - 15000000, 0.9, 1});
  chunk_words[1].push_back({"beta", 22000000 - 15000000, 23000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 2);
  assert(reconciled[0].text == "alpha");
  assert(reconciled[0].start_us == 16000000);
  assert(reconciled[1].text == "beta");
  assert(reconciled[1].start_us == 22000000);
}

void test_overlap_reconciliation_word_start_equals_prior_end_at_boundary() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  chunk_words[0].push_back({"first", 16000000, 17000000, 0.9, 0});
  chunk_words[1].push_back({"first", 16000000 - 15000000, 17000000 - 15000000, 0.9, 1});
  chunk_words[1].push_back({"second", 17000000 - 15000000, 18000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 2);
  assert(reconciled[0].text == "first");
  assert(reconciled[0].start_us == 16000000);
  assert(reconciled[0].end_us == 17000000);
  assert(reconciled[1].text == "second");
  assert(reconciled[1].start_us == 17000000);
  assert(reconciled[1].end_us == 18000000);
}

void test_overlap_reconciliation_shifted_token_inside_overlap() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  chunk_words[0].push_back({"original", 17000000, 17500000, 0.9, 0});
  chunk_words[1].push_back({"shifted", 17200000 - 15000000, 17700000 - 15000000, 0.85, 1});
  chunk_words[1].push_back({"after", 21000000 - 15000000, 22000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 2);
  assert(reconciled[0].text == "original");
  assert(reconciled[0].start_us == 17000000);
  assert(reconciled[1].text == "after");
  assert(reconciled[1].start_us == 21000000);
}

void test_overlap_reconciliation_no_false_dedepe_outside_overlap() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);
  assert(plan.chunks[1].source_start_us == 15000000);
  assert(plan.chunks[1].overlap_before_us == 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  chunk_words[0].push_back({"early", 1000000, 2000000, 0.9, 0});
  chunk_words[0].push_back({"mid", 12000000, 13000000, 0.9, 0});
  chunk_words[1].push_back({"mid_dup", 12000000 - 15000000, 13000000 - 15000000, 0.9, 1});
  chunk_words[1].push_back({"late", 25000000 - 15000000, 26000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 3);
  assert(reconciled[0].text == "early");
  assert(reconciled[0].start_us == 1000000);
  assert(reconciled[1].text == "mid");
  assert(reconciled[1].start_us == 12000000);
  assert(reconciled[2].text == "late");
  assert(reconciled[2].start_us == 25000000);
}

void test_asr_model_present_vs_verified_distinction() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-asr-model-verify-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "model_whisper_small_en");

  const std::string model_id = "model_whisper_small_en";

  assert(svp::audio::check_asr_model_in_cache(model_id, root) == false);
  assert(svp::audio::verify_asr_model_files(model_id, root) == false);

  {
    std::ofstream manifest(root / "model_whisper_small_en" / "model.svpmodel.json");
    manifest << R"({"schema_version":"svp-model-bundle-1",)"
             << R"("model_bundle_id":"model_whisper_small_en@v1+blake3_000000000000",)"
             << R"("model_id":")" << model_id << R"(","model_version":"v1",)"
             << R"("bundle_blake3":"blake3:0000000000000000000000000000000000000000000000000000000000000000",)"
             << R"("runtime":"onnxruntime","format":"onnx","license":"MIT",)"
             << R"("supported_execution_providers":["cpu"],)"
             << R"("files":[{"path":"model.onnx","role":"model",)"
             << R"("blake3":"blake3:0000000000000000000000000000000000000000000000000000000000000000"}],)"
             << R"("input_contract":{},"output_contract":{},)"
             << R"("preprocessor_contract":{},"postprocessor_contract":{}})";
  }

  assert(svp::audio::check_asr_model_in_cache(model_id, root) == true);
  assert(svp::audio::verify_asr_model_files(model_id, root) == false);

  {
    std::ofstream model_file(root / "model_whisper_small_en" / "model.onnx");
    model_file << "dummy";
  }

  assert(svp::audio::check_asr_model_in_cache(model_id, root) == true);
  assert(svp::audio::verify_asr_model_files(model_id, root) == true);

  std::filesystem::remove_all(root);
}

void test_overlap_reconciliation_empty_chunks() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);
  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);
  assert(reconciled.empty());
}

void test_asr_execution_boundary_blocked_when_model_missing() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, false, false);
  const nlohmann::json encoded =
      svp::audio::asr_execution_boundary_to_json(boundary);

  assert(encoded["asr_status"] == "blocked");
  assert(encoded["analysis_audio_available"] == true);
  assert(encoded["model_runtime_available"] == true);
  assert(encoded["model_available"] == false);
  assert(encoded["model_verified"] == false);
  assert(!encoded["blockers"].empty());
}

void test_asr_execution_boundary_blocked_when_runtime_missing() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, false, false, false);
  assert(boundary.asr_status == svp::audio::AsrStatus::blocked);
  assert(!boundary.blockers.empty());
}

void test_asr_execution_boundary_planned_when_all_available() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  assert(boundary.asr_status == svp::audio::AsrStatus::planned);
  assert(boundary.blockers.empty());
}

void test_asr_execution_boundary_json_reports_decoder_token_softmax_mean() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  const nlohmann::json encoded =
      svp::audio::asr_execution_boundary_to_json(boundary);

  assert(encoded["asr_limitations"]["confidence_status"] == "decoder_token_softmax_mean");
  std::string note = encoded["asr_limitations"]["confidence_note"];
  assert(note.find("uncalibrated") != std::string::npos);
}

void test_transcript_writer_produces_honest_blocked_absence() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-asr-transcript-blocked-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, false, false, false);

  const svp::audio::TranscriptWriteResult result =
      svp::audio::write_transcript_artifacts(boundary, root);
  const nlohmann::json encoded =
      svp::audio::transcript_write_result_to_json(result);

  assert(encoded["transcript_written"] == true);
  assert(encoded["words_written"] == true);
  assert(encoded["speakers_written"] == true);
  assert(encoded["chunk_provenance_written"] == true);
  assert(encoded["word_count"] == 0);
  assert(encoded["speaker_count"] == 0);
  assert(encoded["transcript_status"] == "blocked");

  assert(std::filesystem::exists(root / "transcript/transcript.json"));
  assert(std::filesystem::exists(root / "transcript/words.jsonl"));
  assert(std::filesystem::exists(root / "transcript/speakers.jsonl"));
  assert(std::filesystem::exists(root / "transcript/asr_chunk_provenance.jsonl"));

  {
    std::ifstream input(root / "transcript/transcript.json");
    const nlohmann::json transcript = nlohmann::json::parse(input);
    assert(transcript["language"]["primary"] == "und");
    assert(transcript["language"]["mode"] == "undetermined");
    assert(transcript["word_count"] == 0);
    assert(transcript["speaker_count"] == 0);
    assert(transcript["asr_status"] == "blocked");
  }

  {
    std::ifstream input(root / "transcript/words.jsonl");
    std::string line;
    assert(!std::getline(input, line));
  }

  {
    std::ifstream input(root / "transcript/asr_chunk_provenance.jsonl");
    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
      const nlohmann::json record = nlohmann::json::parse(line);
      assert(record["asr_status"] == "blocked");
      assert(record["chunk_id"].get<std::string>().substr(0, 10) == "asr_chunk_");
      ++count;
    }
    assert(count == 2);
  }

  std::filesystem::remove_all(root);
}

void test_transcript_writer_produces_honest_zero_duration_absence() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-asr-transcript-zero-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(0, 30000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, false, false, false, false);

  const svp::audio::TranscriptWriteResult result =
      svp::audio::write_transcript_artifacts(boundary, root);

  assert(result.transcript_written);
  assert(result.words_written);
  assert(result.speakers_written);
  assert(result.chunk_provenance_written);
  assert(result.word_count == 0);

  {
    std::ifstream input(root / "transcript/transcript.json");
    const nlohmann::json transcript = nlohmann::json::parse(input);
    assert(transcript["word_count"] == 0);
    assert(transcript["duration_us"] == 0);
  }

  {
    std::ifstream input(root / "transcript/asr_chunk_provenance.jsonl");
    std::string line;
    assert(!std::getline(input, line));
  }

  std::filesystem::remove_all(root);
}

void test_whisper_runtime_available_reports_honestly() {
  const bool available = svp::audio::is_whisper_runtime_available();
#ifdef SVP_AUDIO_ONNX_RUNTIME_AVAILABLE
  assert(available);
#else
  assert(!available);
#endif
}

void test_whisper_inference_blocks_when_model_dir_missing() {
  const std::filesystem::path fake_dir =
      std::filesystem::temp_directory_path() / "svp-whisper-fake-model";
  std::filesystem::remove_all(fake_dir);
  std::filesystem::create_directories(fake_dir);

  const std::filesystem::path fake_wav = fake_dir / "test.wav";
  {
    std::ofstream output(fake_wav, std::ios::binary);
    write_u32_le(output, 0x46464952);
    write_u32_le(output, 36 + 16000 * 2);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    write_u32_le(output, 16);
    write_u16_le(output, 1);
    write_u16_le(output, 1);
    write_u32_le(output, 16000);
    write_u32_le(output, 32000);
    write_u16_le(output, 2);
    write_u16_le(output, 16);
    output.write("data", 4);
    write_u32_le(output, 16000 * 2);
    for (int i = 0; i < 16000; ++i) {
      write_u16_le(output, 0);
    }
  }

  const svp::audio::WhisperInferenceResult result =
      svp::audio::run_whisper_inference(fake_wav, fake_dir, "test_chunk", 0, 1000000);

  assert(!result.ran);
  assert(!result.blockers.empty());

  std::filesystem::remove_all(fake_dir);
}

void test_diarization_boundary_fallback_when_model_unavailable() {
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  assert(boundary.diarization_status == svp::audio::DiarizationStatus::unavailable);
  assert(!boundary.blockers.empty());

  const svp::audio::DiarizationExecutionBoundary executed =
      svp::audio::execute_diarization_boundary(std::move(boundary), "", "");
  assert(executed.diarization_status == svp::audio::DiarizationStatus::fallback_one_speaker);
  assert(executed.speaker_count == 1);
  assert(executed.speaker_segments.size() == 1);
  assert(executed.speaker_segments[0].speaker_id == "speaker_0001");
  assert(executed.speaker_segments[0].timing.start_us == 0);
  assert(executed.speaker_segments[0].timing.end_us == 30000000);
  assert(executed.speaker_segments[0].overlap == false);
}

void test_diarization_boundary_unavailable_when_no_audio() {
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          false, false, false, false, 0);
  assert(boundary.diarization_status == svp::audio::DiarizationStatus::unavailable);

  const svp::audio::DiarizationExecutionBoundary executed =
      svp::audio::execute_diarization_boundary(std::move(boundary), "", "");
  assert(executed.diarization_status == svp::audio::DiarizationStatus::unavailable);
  assert(executed.speaker_segments.empty());
  assert(executed.speaker_count == 0);
}

void test_diarization_boundary_json_serialization() {
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  boundary = svp::audio::execute_diarization_boundary(std::move(boundary), "", "");
  const nlohmann::json json =
      svp::audio::diarization_execution_boundary_to_json(boundary);

  assert(json["diarization_status"] == "fallback_one_speaker");
  assert(json["speaker_count"] == 1);
  assert(json["speaker_segments"].size() == 1);
  assert(json["speaker_segments"][0]["speaker_id"] == "speaker_0001");
  assert(json["speaker_segments"][0]["start_us"] == 0);
  assert(json["speaker_segments"][0]["end_us"] == 30000000);
  assert(json["speaker_segments"][0]["overlap"] == false);
}

void test_transcript_writer_writes_speaker_segments_with_fallback() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-diar-transcript-fallback-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.reconciled_words.push_back({"hello", 1000000, 1500000, 0.9, 0});
  boundary.reconciled_word_count = 1;
  boundary.speaker_count = 1;
  boundary.one_speaker_mode = true;
  boundary.diarization_status = "fallback_one_speaker";

  svp::audio::SpeakerSegment fallback_segment;
  fallback_segment.id = "speakerseg_000000";
  fallback_segment.speaker_id = "speaker_0001";
  fallback_segment.timing = {0, 30000000};
  fallback_segment.confidence = 0.0;
  fallback_segment.overlap = false;
  boundary.speaker_segments.push_back(std::move(fallback_segment));

  const svp::audio::TranscriptWriteResult result =
      svp::audio::write_transcript_artifacts(boundary, root);

  assert(result.transcript_written);
  assert(result.words_written);
  assert(result.speakers_written);
  assert(result.speaker_segments_written);
  assert(result.word_count == 1);
  assert(result.speaker_count == 1);

  {
    std::ifstream input(root / "transcript/transcript.json");
    const nlohmann::json transcript = nlohmann::json::parse(input);
    assert(transcript["diarization"]["status"] == "fallback_one_speaker");
    assert(transcript["diarization"]["one_speaker_fallback"] == true);
    assert(transcript["asr_limitations"]["speaker_mode"] == "one_speaker_fallback");
  }

  {
    std::ifstream input(root / "transcript/words.jsonl");
    std::string line;
    std::getline(input, line);
    const nlohmann::json word = nlohmann::json::parse(line);
    assert(word["speaker_id"] == "speaker_0001");
  }

  {
    std::ifstream input(root / "transcript/speakers.jsonl");
    std::string line;
    std::getline(input, line);
    const nlohmann::json speaker = nlohmann::json::parse(line);
    assert(speaker["id"] == "speaker_0001");
    assert(speaker["diarization_status"] == "fallback_one_speaker");
  }

  {
    std::ifstream input(root / "transcript/speaker_segments.jsonl");
    std::string line;
    std::getline(input, line);
    const nlohmann::json seg = nlohmann::json::parse(line);
    assert(seg["id"] == "speakerseg_000000");
    assert(seg["speaker_id"] == "speaker_0001");
    assert(seg["start_us"] == 0);
    assert(seg["end_us"] == 30000000);
    assert(seg["overlap"] == false);
  }

  std::filesystem::remove_all(root);
}

void test_transcript_writer_blocked_includes_diarization_provenance() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-diar-transcript-blocked-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, false, false, false);
  boundary.diarization_status = "unavailable";

  const svp::audio::TranscriptWriteResult result =
      svp::audio::write_transcript_artifacts(boundary, root);

  assert(result.transcript_written);
  assert(result.transcript_status == "blocked");

  {
    std::ifstream input(root / "transcript/transcript.json");
    const nlohmann::json transcript = nlohmann::json::parse(input);
    assert(transcript["diarization"]["status"] == "unavailable");
    assert(transcript["diarization"]["one_speaker_fallback"] == false);
  }

  std::filesystem::remove_all(root);
}

void test_blocked_asr_with_fallback_segments_does_not_create_dangling_speaker_segments() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-diar-blocked-no-dangling-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, false, false, false);
  boundary.asr_status = svp::audio::AsrStatus::blocked;
  boundary.diarization_status = "fallback_one_speaker";
  boundary.diarization_note = "One-speaker fallback used (model missing). This is not speaker recognition.";
  boundary.diarization_blockers = {"model missing"};

  // Even though speaker_segments are populated, blocked ASR must not
  // write them — that would create dangling references to speaker_0001
  // while speakers.jsonl is intentionally empty.
  svp::audio::SpeakerSegment fallback_segment;
  fallback_segment.id = "speakerseg_000000";
  fallback_segment.speaker_id = "speaker_0001";
  fallback_segment.timing = {0, 30000000};
  fallback_segment.confidence = 0.0;
  fallback_segment.overlap = false;
  boundary.speaker_segments.push_back(std::move(fallback_segment));

  const svp::audio::TranscriptWriteResult result =
      svp::audio::write_transcript_artifacts(boundary, root);

  assert(result.transcript_written);
  assert(result.transcript_status == "blocked");
  assert(result.speaker_count == 0);

  // speakers.jsonl must be empty
  {
    std::ifstream input(root / "transcript/speakers.jsonl");
    std::string content((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    assert(content.empty());
  }

  // speaker_segments.jsonl must also be empty — no dangling references
  {
    std::ifstream input(root / "transcript/speaker_segments.jsonl");
    std::string content((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    assert(content.empty());
  }

  std::filesystem::remove_all(root);
}

void test_fallback_provenance_distinguishes_model_missing_from_inference_not_wired() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-diar-provenance-wording-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  // Case 1: model missing
  {
    svp::audio::AsrExecutionBoundary boundary =
        svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
    boundary.asr_status = svp::audio::AsrStatus::ran;
    boundary.reconciled_words.push_back({"hello", 1000000, 1500000, 0.9, 0});
    boundary.reconciled_word_count = 1;
    boundary.speaker_count = 1;
    boundary.one_speaker_mode = true;
    boundary.diarization_status = "fallback_one_speaker";
    boundary.diarization_blockers = {"sherpa-onnx diarization model is not available in model cache"};
    boundary.diarization_note = "One-speaker fallback used (sherpa-onnx diarization model is not available in model cache). This is not speaker recognition.";

    svp::audio::SpeakerSegment seg;
    seg.id = "speakerseg_000000";
    seg.speaker_id = "speaker_0001";
    seg.timing = {0, 30000000};
    boundary.speaker_segments.push_back(std::move(seg));

    svp::audio::write_transcript_artifacts(boundary, root);

    std::ifstream input(root / "transcript/transcript.json");
    const nlohmann::json transcript = nlohmann::json::parse(input);
    assert(transcript["diarization"]["status"] == "fallback_one_speaker");
    std::string note = transcript["diarization"]["note"];
    assert(note.find("model is not available") != std::string::npos);
    assert(note.find("not speaker recognition") != std::string::npos);
    assert(transcript["diarization"]["blockers"].size() == 1);
  }

  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  // Case 2: model present but inference not wired
  {
    svp::audio::AsrExecutionBoundary boundary =
        svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
    boundary.asr_status = svp::audio::AsrStatus::ran;
    boundary.reconciled_words.push_back({"hello", 1000000, 1500000, 0.9, 0});
    boundary.reconciled_word_count = 1;
    boundary.speaker_count = 1;
    boundary.one_speaker_mode = true;
    boundary.diarization_status = "fallback_one_speaker";
    boundary.diarization_blockers = {"sherpa-onnx diarization model inference is not yet wired; fallback one-speaker segment emitted"};
    boundary.diarization_note = "One-speaker fallback used (sherpa-onnx diarization model inference is not yet wired; fallback one-speaker segment emitted). This is not speaker recognition.";

    svp::audio::SpeakerSegment seg;
    seg.id = "speakerseg_000000";
    seg.speaker_id = "speaker_0001";
    seg.timing = {0, 30000000};
    boundary.speaker_segments.push_back(std::move(seg));

    svp::audio::write_transcript_artifacts(boundary, root);

    std::ifstream input(root / "transcript/transcript.json");
    const nlohmann::json transcript = nlohmann::json::parse(input);
    assert(transcript["diarization"]["status"] == "fallback_one_speaker");
    std::string note = transcript["diarization"]["note"];
    assert(note.find("inference is not yet wired") != std::string::npos);
    assert(note.find("not speaker recognition") != std::string::npos);
    // Must NOT say "model unavailable"
    assert(note.find("model unavailable") == std::string::npos);
  }

  std::filesystem::remove_all(root);
}

// ---- Reconciliation unit tests ----

void test_reconcile_single_pair_low_similarity_keeps_two_speakers() {
  // Two clusters with low similarity (0.2) should remain 2 speakers.
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.2f},
      {0.2f, 1.0f}
  };
  std::vector<int32_t> cluster_ids = {0, 1};

  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);

  assert(result.final_speaker_count == 2);
  assert(result.merge_decisions.size() == 1);
  assert(result.merge_decisions[0].merged == false);
  assert(result.merge_decisions[0].cosine_similarity == 0.2f);
  assert(result.cluster_to_final.at(0) != result.cluster_to_final.at(1));
}

void test_reconcile_single_pair_high_similarity_merges_to_one() {
  // Two clusters with high similarity (0.8) should merge to 1 speaker.
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.8f},
      {0.8f, 1.0f}
  };
  std::vector<int32_t> cluster_ids = {0, 1};

  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);

  assert(result.final_speaker_count == 1);
  assert(result.merge_decisions.size() == 1);
  assert(result.merge_decisions[0].merged == true);
  assert(result.merge_decisions[0].cosine_similarity == 0.8f);
  assert(result.cluster_to_final.at(0) == result.cluster_to_final.at(1));
}

void test_reconcile_three_cluster_largest_gap_keeps_two_speakers() {
  // 3 clusters with similarities [0.456, 0.101, -0.045].
  // Largest gap = 0.356 (between 0.456 and 0.101), above 0.25 min-gap.
  // Clusters 0&1 merge, cluster 2 stays separate -> 2 final speakers.
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.456f, 0.101f},
      {0.456f, 1.0f, -0.045f},
      {0.101f, -0.045f, 1.0f}
  };
  std::vector<int32_t> cluster_ids = {0, 1, 2};

  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);

  assert(result.final_speaker_count == 2);
  assert(result.merge_decisions.size() == 3);
  // Highest sim pair (0&1, sim=0.456) should merge
  bool found_merge = false;
  bool found_no_merge = false;
  for (const auto& md : result.merge_decisions) {
    if (md.merged) found_merge = true;
    if (!md.merged) found_no_merge = true;
  }
  assert(found_merge);
  assert(found_no_merge);
}

void test_reconcile_three_cluster_min_gap_merges_all_to_one() {
  // 3 clusters with similarities [0.649, 0.471, 0.343].
  // Largest gap = 0.178, below 0.25 min-gap threshold.
  // All clusters merge -> 1 final speaker.
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.471f, 0.649f},
      {0.471f, 1.0f, 0.343f},
      {0.649f, 0.343f, 1.0f}
  };
  std::vector<int32_t> cluster_ids = {0, 1, 2};

  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);

  assert(result.final_speaker_count == 1);
  assert(result.merge_decisions.size() == 3);
  for (const auto& md : result.merge_decisions) {
    assert(md.merged == true);
  }
  assert(result.cluster_to_final.at(0) == result.cluster_to_final.at(1));
  assert(result.cluster_to_final.at(1) == result.cluster_to_final.at(2));
}

void test_whisper_mel_30s_chunk_produces_valid_output_without_overread() {
  // A 30-second chunk at 16 kHz is exactly 480000 samples.
  // The STFT loop needs (kNFrames-1)*kNHop + kNFft = 480240 samples.
  // The old code resized to 480000, causing a 240-sample buffer overread
  // in the last STFT frame (t=2999, start=479840, reads through 480239).
  std::vector<std::int16_t> samples(480000, 16384);
  const auto wav_path =
      std::filesystem::temp_directory_path() / "svp_mel_test_30s.wav";
  write_pcm_s16le_mono_wav(wav_path, samples);

  svp::audio::WhisperMelFeatures features =
      svp::audio::compute_whisper_mel_from_wav(wav_path);
  assert(features.n_mels == 80);
  assert(features.n_frames == 3000);
  assert(static_cast<int>(features.data.size()) == 80 * 3000);

  for (float v : features.data) {
    assert(std::isfinite(v));
  }

  // The last non-tail-padded frame (t=2949) must carry signal from real
  // audio samples. With the buffer fix, all STFT reads are in-bounds.
  // The last 50 frames (2950-2999) are zeroed for tail padding (EOT
  // detection), so we check frame 2949 instead of 2999.
  float max_last_audio_frame = -std::numeric_limits<float>::max();
  for (int m = 0; m < 80; ++m) {
    max_last_audio_frame =
        std::max(max_last_audio_frame, features.data[m * 3000 + 2949]);
  }
  assert(max_last_audio_frame > 1.5f);

  // Tail-padded frames must be exactly 0.0.
  for (int m = 0; m < 80; ++m) {
    assert(features.data[m * 3000 + 2999] == 0.0f);
  }

  std::filesystem::remove(wav_path);
}

void test_whisper_mel_buffer_includes_samples_for_last_stft_frame() {
  // The old code resized the audio buffer to 480000 samples. The STFT loop
  // needs 480240 samples for all 3000 frames. Without the fix, frame 2999
  // reads 240 samples past the buffer end (undefined behavior).
  // This test verifies the function completes safely with exactly 480000
  // samples (the old buffer size) and produces valid output.
  std::vector<std::int16_t> samples(480000, 16384);
  const auto wav_path =
      std::filesystem::temp_directory_path() / "svp_mel_test_tail.wav";
  write_pcm_s16le_mono_wav(wav_path, samples);

  svp::audio::WhisperMelFeatures features =
      svp::audio::compute_whisper_mel_from_wav(wav_path);
  assert(features.n_mels == 80);
  assert(features.n_frames == 3000);

  // All values must be finite (no NaN/inf from buffer overread garbage).
  for (float v : features.data) {
    assert(std::isfinite(v));
  }

  // The last non-tail-padded frame (t=2949) must carry signal.
  float max_last_audio_frame = -std::numeric_limits<float>::max();
  for (int m = 0; m < 80; ++m) {
    max_last_audio_frame =
        std::max(max_last_audio_frame, features.data[m * 3000 + 2949]);
  }
  assert(max_last_audio_frame > 0.5f);

  // Tail-padded frames (t=2950..2999) must be exactly 0.0.
  for (int t = 2950; t < 3000; ++t) {
    for (int m = 0; m < 80; ++m) {
      assert(features.data[m * 3000 + t] == 0.0f);
    }
  }

  std::filesystem::remove(wav_path);
}

void test_whisper_mel_uses_log10_for_compression() {
  // Whisper and sherpa-onnx both use log10 (not natural log) for mel
  // power compression. For a constant signal of amplitude ~0.5, the DC
  // bin power is roughly (0.5 * 200)^2 = 10000. After log10: log10(10000) = 4.0.
  // The final normalization is (val + 4) / 4, giving:
  //   log10: (4.0 + 4) / 4 = 2.0
  //   ln:    (9.21 + 4) / 4 ≈ 3.30
  // A threshold of 2.5 confirms log10 is used (max ≈ 2.0, not 3.3).
  std::vector<std::int16_t> samples(16000, 16384);
  const auto wav_path =
      std::filesystem::temp_directory_path() / "svp_mel_test_log.wav";
  write_pcm_s16le_mono_wav(wav_path, samples);

  svp::audio::WhisperMelFeatures features =
      svp::audio::compute_whisper_mel_from_wav(wav_path);
  assert(features.n_mels == 80);
  assert(features.n_frames == 3000);

  float max_val = -std::numeric_limits<float>::max();
  for (float v : features.data) {
    max_val = std::max(max_val, v);
  }

  // With log10, max_val should be ≈2.0. With natural log, ≈3.3.
  // Threshold of 2.5 confirms log10 is used.
  assert(max_val < 2.5f);

  std::filesystem::remove(wav_path);
}

void test_softmax_probability_for_token_basic() {
  // Two logits: token 0 has logit 0, token 1 has logit 0.
  // Softmax should give 0.5 for each.
  std::vector<float> logits = {0.0f, 0.0f};
  double p0 = svp::audio::softmax_probability_for_token(logits, 0);
  double p1 = svp::audio::softmax_probability_for_token(logits, 1);
  assert(std::abs(p0 - 0.5) < 1e-9);
  assert(std::abs(p1 - 0.5) < 1e-9);

  // Token 0 has much higher logit -> probability close to 1.
  logits = {10.0f, 0.0f};
  p0 = svp::audio::softmax_probability_for_token(logits, 0);
  p1 = svp::audio::softmax_probability_for_token(logits, 1);
  assert(p0 > 0.9999);
  assert(p1 < 0.0001);
  assert(p0 + p1 > 0.9999 && p0 + p1 < 1.0001);
}

void test_softmax_probability_for_token_edge_cases() {
  // Empty logits -> 0.0
  std::vector<float> empty;
  assert(svp::audio::softmax_probability_for_token(empty, 0) == 0.0);

  // Out-of-range token_id -> 0.0
  std::vector<float> logits = {1.0f, 2.0f, 3.0f};
  assert(svp::audio::softmax_probability_for_token(logits, -1) == 0.0);
  assert(svp::audio::softmax_probability_for_token(logits, 3) == 0.0);

  // Single token -> probability 1.0
  logits = {5.0f};
  double p = svp::audio::softmax_probability_for_token(logits, 0);
  assert(std::abs(p - 1.0) < 1e-9);
}

void test_aggregate_word_confidence_mean() {
  std::vector<double> token_probs = {0.8, 0.6, 0.9, 0.3};
  // Mean of tokens 0,1,2 = (0.8+0.6+0.9)/3 = 0.7666...
  std::vector<std::size_t> indices = {0, 1, 2};
  double conf = svp::audio::aggregate_word_confidence(token_probs, indices);
  assert(std::abs(conf - (0.8 + 0.6 + 0.9) / 3.0) < 1e-9);

  // Single token
  indices = {3};
  conf = svp::audio::aggregate_word_confidence(token_probs, indices);
  assert(std::abs(conf - 0.3) < 1e-9);
}

void test_aggregate_word_confidence_empty() {
  std::vector<double> token_probs;
  std::vector<std::size_t> indices;
  assert(svp::audio::aggregate_word_confidence(token_probs, indices) == 0.0);

  token_probs = {0.5, 0.7};
  indices = {};
  assert(svp::audio::aggregate_word_confidence(token_probs, indices) == 0.0);

  indices = {5};  // out of range
  assert(svp::audio::aggregate_word_confidence(token_probs, indices) == 0.0);
}

void test_speaker_total_speech_us_nonzero_for_single_speaker() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-speaker-speech-us-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.one_speaker_mode = true;
  boundary.diarization_status = "fallback_one_speaker";
  boundary.speaker_count = 1;

  // Words spanning 1s to 7s with a gap from 3s to 5s.
  // Union = [1s,3s) + [5s,7s) = 2s + 2s = 4s = 4000000us.
  boundary.reconciled_words.push_back({"hello", 1000000, 3000000, 0.9, 0});
  boundary.reconciled_words.push_back({"world", 5000000, 7000000, 0.85, 0});
  boundary.reconciled_word_count = 2;

  svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/speakers.jsonl");
  std::string line;
  std::getline(input, line);
  const nlohmann::json speaker = nlohmann::json::parse(line);
  assert(speaker["id"] == "speaker_0001");
  assert(speaker["total_speech_us"] == 4000000);

  std::filesystem::remove_all(root);
}

void test_speaker_total_speech_us_overlapping_not_double_counted() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-speaker-overlap-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.one_speaker_mode = true;
  boundary.diarization_status = "fallback_one_speaker";
  boundary.speaker_count = 1;

  // Overlapping words: [1s,4s), [2s,5s), [3s,6s)
  // Union = [1s,6s) = 5s = 5000000us (not 9s).
  boundary.reconciled_words.push_back({"alpha", 1000000, 4000000, 0.9, 0});
  boundary.reconciled_words.push_back({"beta", 2000000, 5000000, 0.85, 0});
  boundary.reconciled_words.push_back({"gamma", 3000000, 6000000, 0.8, 0});
  boundary.reconciled_word_count = 3;

  svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/speakers.jsonl");
  std::string line;
  std::getline(input, line);
  const nlohmann::json speaker = nlohmann::json::parse(line);
  assert(speaker["total_speech_us"] == 5000000);

  std::filesystem::remove_all(root);
}

void test_speaker_total_speech_us_multi_speaker() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-speaker-multi-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.one_speaker_mode = false;
  boundary.diarization_status = "ran";
  boundary.speaker_count = 2;

  // Speaker 1: words "hello" at [1s,2s) and "foo" at [2s,4s) -> union = [1s,4s) = 3s
  // Speaker 2: word "world" at [5s,7s) -> union = 2s
  boundary.reconciled_words.push_back({"hello", 1000000, 2000000, 0.9, 0});
  boundary.reconciled_words.push_back({"world", 5000000, 7000000, 0.85, 0});
  boundary.reconciled_words.push_back({"foo", 2000000, 4000000, 0.8, 0});
  boundary.reconciled_word_count = 3;

  svp::audio::SpeakerSegment seg1;
  seg1.id = "speakerseg_000000";
  seg1.speaker_id = "speaker_0001";
  seg1.timing = {0, 4000000};
  boundary.speaker_segments.push_back(std::move(seg1));

  svp::audio::SpeakerSegment seg2;
  seg2.id = "speakerseg_000001";
  seg2.speaker_id = "speaker_0002";
  seg2.timing = {4000000, 30000000};
  boundary.speaker_segments.push_back(std::move(seg2));

  svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/speakers.jsonl");
  std::string line;
  std::getline(input, line);
  const nlohmann::json speaker1 = nlohmann::json::parse(line);
  assert(speaker1["id"] == "speaker_0001");
  assert(speaker1["total_speech_us"] == 3000000);

  std::getline(input, line);
  const nlohmann::json speaker2 = nlohmann::json::parse(line);
  assert(speaker2["id"] == "speaker_0002");
  assert(speaker2["total_speech_us"] == 2000000);

  std::filesystem::remove_all(root);
}

void test_transcript_confidence_provenance_is_decoder_token_softmax_mean() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-confidence-provenance-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.one_speaker_mode = true;
  boundary.diarization_status = "fallback_one_speaker";
  boundary.speaker_count = 1;
  boundary.reconciled_words.push_back({"hello", 1000000, 2000000, 0.87, 0});
  boundary.reconciled_word_count = 1;

  svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/transcript.json");
  const nlohmann::json transcript = nlohmann::json::parse(input);
  assert(transcript["asr_limitations"]["confidence_status"] == "decoder_token_softmax_mean");
  std::string note = transcript["asr_limitations"]["confidence_note"];
  assert(note.find("uncalibrated") != std::string::npos);

  std::ifstream winput(root / "transcript/words.jsonl");
  std::string line;
  std::getline(winput, line);
  const nlohmann::json word = nlohmann::json::parse(line);
  assert(word["confidence"] == 0.87);

  std::filesystem::remove_all(root);
}

}  // namespace

void test_set_sherpa_lib_path_with_invalid_path_leaves_unavailable() {
  svp::audio::set_sherpa_lib_path("/nonexistent/path/to/libsherpa-onnx-c-api.dylib");
  bool available = svp::audio::is_sherpa_diarization_available();
  assert(!available);

  std::vector<std::string> attempted = svp::audio::sherpa_lib_paths_attempted();
  assert(!attempted.empty());
  bool found_explicit = false;
  for (const auto& p : attempted) {
    if (p.find("/nonexistent/path/to/libsherpa-onnx-c-api.dylib") != std::string::npos) {
      found_explicit = true;
      break;
    }
  }
  assert(found_explicit);
}

void test_multi_candidate_search_does_not_crash_when_no_candidate_exists() {
  svp::audio::set_sherpa_lib_path("/definitely/not/here/libsherpa-onnx-c-api.dylib");
  bool available = svp::audio::is_sherpa_diarization_available();
  assert(!available);

  std::string used = svp::audio::sherpa_lib_path_used();
  assert(used.empty());

  std::vector<std::string> attempted = svp::audio::sherpa_lib_paths_attempted();
  assert(!attempted.empty());
}

void test_reconcile_clusters_still_works_after_lib_discovery() {
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.3f},
      {0.3f, 1.0f},
  };
  std::vector<int32_t> cluster_ids = {0, 1};
  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);
  assert(result.final_speaker_count == 2);
  assert(result.cluster_to_final.size() == 2);
  assert(result.cluster_to_final[0] == 0);
  assert(result.cluster_to_final[1] == 1);
}

int main() {
  test_word_serialization_uses_canonical_time_strings();
  test_zero_duration_span_is_rejected_for_core_span_records();
  test_attached_punctuation_can_have_zero_duration();
  test_audio_stage_plan_is_honest_about_pending_processors();
  test_audio_extraction_plan_documents_ffmpeg_commands_when_available();
  test_multi_stream_analysis_audio_selects_first_stream();
  test_waveform_envelope_generates_ten_millisecond_json_records();
  test_audio_extraction_executor_writes_staged_single_stream_outputs();
  test_audio_extraction_executor_leaves_multi_stream_analysis_unrun();
  test_vad_task_plan_uses_stable_thirty_second_boundaries();
  test_vad_execution_boundary_preserves_honest_unrun_state();
  test_execute_vad_boundary_handles_runtime_unavailable_honestly();
  test_asr_chunk_plan_produces_correct_overlapping_chunks();
  test_asr_chunk_plan_zero_duration_produces_no_chunks();
  test_asr_chunk_plan_rejects_bad_parameters();
  test_asr_chunk_plan_exact_multiple_has_no_trailing_chunk();
  test_overlap_reconciliation_deduplicates_boundary_words();
  test_overlap_reconciliation_duplicate_in_actual_overlap_region();
  test_overlap_reconciliation_word_start_equals_prior_end_at_boundary();
  test_overlap_reconciliation_shifted_token_inside_overlap();
  test_overlap_reconciliation_no_false_dedepe_outside_overlap();
  test_overlap_reconciliation_empty_chunks();
  test_asr_model_present_vs_verified_distinction();
  test_asr_execution_boundary_blocked_when_model_missing();
  test_asr_execution_boundary_blocked_when_runtime_missing();
  test_asr_execution_boundary_planned_when_all_available();
  test_asr_execution_boundary_json_reports_decoder_token_softmax_mean();
  test_transcript_writer_produces_honest_blocked_absence();
  test_transcript_writer_produces_honest_zero_duration_absence();
  test_whisper_runtime_available_reports_honestly();
  test_whisper_inference_blocks_when_model_dir_missing();
  test_diarization_boundary_fallback_when_model_unavailable();
  test_diarization_boundary_unavailable_when_no_audio();
  test_diarization_boundary_json_serialization();
  test_transcript_writer_writes_speaker_segments_with_fallback();
  test_transcript_writer_blocked_includes_diarization_provenance();
  test_blocked_asr_with_fallback_segments_does_not_create_dangling_speaker_segments();
  test_fallback_provenance_distinguishes_model_missing_from_inference_not_wired();

  // Reconciliation tests
  test_reconcile_single_pair_low_similarity_keeps_two_speakers();
  test_reconcile_single_pair_high_similarity_merges_to_one();
  test_reconcile_three_cluster_largest_gap_keeps_two_speakers();
  test_reconcile_three_cluster_min_gap_merges_all_to_one();

  // Whisper mel preprocessing tests
  test_whisper_mel_30s_chunk_produces_valid_output_without_overread();
  test_whisper_mel_buffer_includes_samples_for_last_stft_frame();
  test_whisper_mel_uses_log10_for_compression();

  // Confidence and speech duration tests
  test_softmax_probability_for_token_basic();
  test_softmax_probability_for_token_edge_cases();
  test_aggregate_word_confidence_mean();
  test_aggregate_word_confidence_empty();
  test_speaker_total_speech_us_nonzero_for_single_speaker();
  test_speaker_total_speech_us_overlapping_not_double_counted();
  test_speaker_total_speech_us_multi_speaker();
  test_transcript_confidence_provenance_is_decoder_token_softmax_mean();

  // Sherpa library discovery tests
  test_set_sherpa_lib_path_with_invalid_path_leaves_unavailable();
  test_multi_candidate_search_does_not_crash_when_no_candidate_exists();
  test_reconcile_clusters_still_works_after_lib_discovery();
  return 0;
}
