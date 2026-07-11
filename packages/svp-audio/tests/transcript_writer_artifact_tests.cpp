#include "audio_test_support.hpp"

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

void test_transcript_writer_ran_with_zero_speakers_and_zero_words() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-diar-ran-zero-speakers-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.reconciled_word_count = 0;
  boundary.speaker_count = 0;
  boundary.one_speaker_mode = false;
  boundary.diarization_status = "ran";

  const svp::audio::TranscriptWriteResult result =
      svp::audio::write_transcript_artifacts(boundary, root);

  assert(result.transcript_written);
  assert(result.transcript_status == "ran");
  assert(result.word_count == 0);
  assert(result.speaker_count == 0);

  {
    std::ifstream input(root / "transcript/transcript.json");
    const nlohmann::json transcript = nlohmann::json::parse(input);
    assert(transcript["diarization"]["status"] == "ran");
    assert(transcript["speaker_count"] == 0);
    assert(transcript["word_count"] == 0);
  }

  {
    std::ifstream input(root / "transcript/words.jsonl");
    std::string content((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    assert(content.empty());
  }

  {
    std::ifstream input(root / "transcript/speakers.jsonl");
    std::string content((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    assert(content.empty());
  }

  {
    std::ifstream input(root / "transcript/speaker_segments.jsonl");
    std::string content((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    assert(content.empty());
  }

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

  (void)svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/transcript.json");
  const nlohmann::json transcript = nlohmann::json::parse(input);
  assert(transcript["asr_limitations"]["confidence_status"] ==
         "whisper_cpp_token_probability_mean");
  std::string note = transcript["asr_limitations"]["confidence_note"];
  assert(note.find("uncalibrated") != std::string::npos);

  std::ifstream winput(root / "transcript/words.jsonl");
  std::string line;
  std::getline(winput, line);
  const nlohmann::json word = nlohmann::json::parse(line);
  assert(word["confidence"] == 0.87);

  std::filesystem::remove_all(root);
}
