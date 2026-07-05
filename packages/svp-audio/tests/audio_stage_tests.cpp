#include "audio_test_support.hpp"

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
