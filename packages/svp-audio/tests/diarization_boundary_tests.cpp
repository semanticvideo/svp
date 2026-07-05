#include "audio_test_support.hpp"

void test_diarization_boundary_fallback_when_model_unavailable() {
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  assert(boundary.diarization_status == svp::audio::DiarizationStatus::unavailable);
  assert(!boundary.blockers.empty());

  // With allow_fallback=true, fallback segment is produced.
  const svp::audio::DiarizationExecutionBoundary executed =
      svp::audio::execute_diarization_boundary(std::move(boundary), "", "", true);
  assert(executed.diarization_status == svp::audio::DiarizationStatus::fallback_one_speaker);
  assert(executed.speaker_count == 1);
  assert(executed.speaker_segments.size() == 1);
  assert(executed.speaker_segments[0].speaker_id == "speaker_0001");
  assert(executed.speaker_segments[0].timing.start_us == 0);
  assert(executed.speaker_segments[0].timing.end_us == 30000000);
  assert(executed.speaker_segments[0].overlap == false);
}

void test_diarization_boundary_no_fallback_when_model_unavailable() {
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  assert(boundary.diarization_status == svp::audio::DiarizationStatus::unavailable);
  assert(!boundary.blockers.empty());

  // With allow_fallback=false (default), no fallback segment is produced.
  const svp::audio::DiarizationExecutionBoundary executed =
      svp::audio::execute_diarization_boundary(std::move(boundary), "", "");
  assert(executed.diarization_status == svp::audio::DiarizationStatus::unavailable);
  assert(executed.speaker_count == 0);
  assert(executed.speaker_segments.empty());
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
  boundary = svp::audio::execute_diarization_boundary(std::move(boundary), "", "", true);
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

    (void)svp::audio::write_transcript_artifacts(boundary, root);

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

    (void)svp::audio::write_transcript_artifacts(boundary, root);

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

void test_force_single_speaker_emits_one_segment_and_one_speaker() {
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);

  const svp::audio::DiarizationExecutionBoundary executed =
      svp::audio::execute_diarization_boundary(
          std::move(boundary), "", "", false, true);

  assert(executed.diarization_status ==
         svp::audio::DiarizationStatus::user_declared_single_speaker);
  assert(executed.speaker_count == 1);
  assert(executed.speaker_segments.size() == 1);
  assert(executed.speaker_segments[0].speaker_id == "speaker_0001");
  assert(executed.speaker_segments[0].timing.start_us == 0);
  assert(executed.speaker_segments[0].timing.end_us == 30000000);
  assert(executed.speaker_segments[0].overlap == false);
  assert(executed.blockers.empty());
}

void test_force_single_speaker_is_distinct_from_fallback() {
  // Fallback path
  svp::audio::DiarizationExecutionBoundary fallback_boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  const svp::audio::DiarizationExecutionBoundary fallback_executed =
      svp::audio::execute_diarization_boundary(
          std::move(fallback_boundary), "", "", true, false);
  assert(fallback_executed.diarization_status ==
         svp::audio::DiarizationStatus::fallback_one_speaker);

  // Force single speaker path
  svp::audio::DiarizationExecutionBoundary force_boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  const svp::audio::DiarizationExecutionBoundary force_executed =
      svp::audio::execute_diarization_boundary(
          std::move(force_boundary), "", "", false, true);
  assert(force_executed.diarization_status ==
         svp::audio::DiarizationStatus::user_declared_single_speaker);

  // They must be different enum values
  assert(fallback_executed.diarization_status !=
         force_executed.diarization_status);

  // String representations must differ
  assert(svp::audio::diarization_status_to_string(
             fallback_executed.diarization_status) == "fallback_one_speaker");
  assert(svp::audio::diarization_status_to_string(
             force_executed.diarization_status) == "user_declared_single_speaker");
}

void test_force_single_speaker_does_not_require_sherpa_availability() {
  // Even with no model, no runtime, no audio — force_single_speaker should
  // still produce one segment spanning the declared duration.
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          false, false, false, false, 5000000);

  const svp::audio::DiarizationExecutionBoundary executed =
      svp::audio::execute_diarization_boundary(
          std::move(boundary), "", "", false, true);

  assert(executed.diarization_status ==
         svp::audio::DiarizationStatus::user_declared_single_speaker);
  assert(executed.speaker_count == 1);
  assert(executed.speaker_segments.size() == 1);
  assert(executed.speaker_segments[0].speaker_id == "speaker_0001");
  assert(executed.speaker_segments[0].timing.start_us == 0);
  assert(executed.speaker_segments[0].timing.end_us == 5000000);
  assert(executed.blockers.empty());
}

void test_force_single_speaker_wins_over_allow_fallback() {
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);

  // Both flags set — force_single_speaker should win
  const svp::audio::DiarizationExecutionBoundary executed =
      svp::audio::execute_diarization_boundary(
          std::move(boundary), "", "", true, true);

  assert(executed.diarization_status ==
         svp::audio::DiarizationStatus::user_declared_single_speaker);
  assert(executed.diarization_status !=
         svp::audio::DiarizationStatus::fallback_one_speaker);
  assert(executed.speaker_count == 1);
  assert(executed.speaker_segments.size() == 1);
  assert(executed.blockers.empty());
}

void test_force_single_speaker_json_reports_user_declared_status() {
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  boundary = svp::audio::execute_diarization_boundary(
      std::move(boundary), "", "", false, true);
  const nlohmann::json json =
      svp::audio::diarization_execution_boundary_to_json(boundary);

  assert(json["diarization_status"] == "user_declared_single_speaker");
  assert(json["speaker_count"] == 1);
  assert(json["speaker_segments"].size() == 1);
  assert(json["speaker_segments"][0]["speaker_id"] == "speaker_0001");
  assert(json["speaker_segments"][0]["start_us"] == 0);
  assert(json["speaker_segments"][0]["end_us"] == 30000000);
}

void test_force_single_speaker_transcript_provenance() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-force-single-speaker-transcript-test";
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
  boundary.diarization_status = "user_declared_single_speaker";
  boundary.diarization_note =
      "User requested single-speaker mode; diarization was intentionally skipped.";

  svp::audio::SpeakerSegment single_segment;
  single_segment.id = "speakerseg_000000";
  single_segment.speaker_id = "speaker_0001";
  single_segment.timing = {0, 30000000};
  single_segment.confidence = 0.0;
  single_segment.overlap = false;
  boundary.speaker_segments.push_back(std::move(single_segment));

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
    assert(transcript["diarization"]["status"] == "user_declared_single_speaker");
    assert(transcript["diarization"]["one_speaker_fallback"] == false);
    assert(transcript["diarization"]["user_declared_single_speaker"] == true);
    std::string note = transcript["diarization"]["note"];
    assert(note.find("User requested single-speaker mode") != std::string::npos);
    assert(note.find("intentionally skipped") != std::string::npos);
    assert(transcript["asr_limitations"]["speaker_mode"] == "user_declared_single_speaker");
    std::string speaker_note = transcript["asr_limitations"]["speaker_note"];
    assert(speaker_note.find("User requested single-speaker mode") != std::string::npos);
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
    assert(speaker["diarization_status"] == "user_declared_single_speaker");
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

  {
    std::ifstream input(root / "transcript/asr_chunk_provenance.jsonl");
    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
      const nlohmann::json record = nlohmann::json::parse(line);
      assert(record["asr_limitations"]["speaker_mode"] == "user_declared_single_speaker");
      ++count;
    }
    assert(count > 0);
  }

  std::filesystem::remove_all(root);
}

void test_normal_sherpa_path_unchanged_when_force_not_set() {
  // When force_single_speaker is false and allow_fallback is false,
  // and model is unavailable, status should be unavailable (not user_declared).
  svp::audio::DiarizationExecutionBoundary boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  assert(boundary.diarization_status ==
         svp::audio::DiarizationStatus::unavailable);

  const svp::audio::DiarizationExecutionBoundary executed =
      svp::audio::execute_diarization_boundary(
          std::move(boundary), "", "", false, false);
  assert(executed.diarization_status ==
         svp::audio::DiarizationStatus::unavailable);
  assert(executed.speaker_count == 0);
  assert(executed.speaker_segments.empty());

  // Fallback path still works when force is not set
  svp::audio::DiarizationExecutionBoundary fallback_boundary =
      svp::audio::build_diarization_boundary(
          true, true, false, false, 30000000);
  const svp::audio::DiarizationExecutionBoundary fallback_executed =
      svp::audio::execute_diarization_boundary(
          std::move(fallback_boundary), "", "", true, false);
  assert(fallback_executed.diarization_status ==
         svp::audio::DiarizationStatus::fallback_one_speaker);
}

void test_set_sherpa_lib_path_with_invalid_path_leaves_unavailable() {
  const bool already_loaded = !svp::audio::sherpa_lib_path_used().empty();
  svp::audio::set_sherpa_lib_path("/nonexistent/path/to/libsherpa-onnx-c-api.dylib");
  bool available = svp::audio::is_sherpa_diarization_available();

  std::vector<std::string> attempted = svp::audio::sherpa_lib_paths_attempted();
  assert(!attempted.empty());
  bool found_explicit = false;
  for (const auto& p : attempted) {
    if (p.find("/nonexistent/path/to/libsherpa-onnx-c-api.dylib") != std::string::npos) {
      found_explicit = true;
      break;
    }
  }
  if (!already_loaded) {
    assert(found_explicit);
  } else {
    assert(available);
  }

  // If sherpa-onnx is installed on this machine, the dynamic discovery
  // may find it through other candidate paths. Only assert unavailable
  // when the library is genuinely not installed.
  if (!available) {
    std::string used = svp::audio::sherpa_lib_path_used();
    assert(used.empty());
  }
}

void test_multi_candidate_search_does_not_crash_when_no_candidate_exists() {
  svp::audio::set_sherpa_lib_path("/definitely/not/here/libsherpa-onnx-c-api.dylib");
  bool available = svp::audio::is_sherpa_diarization_available();

  std::vector<std::string> attempted = svp::audio::sherpa_lib_paths_attempted();
  assert(!attempted.empty());

  // If sherpa-onnx is installed on this machine, the dynamic discovery
  // may find it through other candidate paths. Only assert unavailable
  // when the library is genuinely not installed.
  if (!available) {
    std::string used = svp::audio::sherpa_lib_path_used();
    assert(used.empty());
  }
}
