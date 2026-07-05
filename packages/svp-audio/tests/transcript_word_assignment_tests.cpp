#include "audio_test_support.hpp"

void test_word_assignment_max_overlap_wins() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-word-assign-overlap-test";
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

  // Word [1s, 3.5s) overlaps seg1 [0, 4s) by 2.5s and seg2 [3s, 5s) by 0.5s.
  // Max overlap should pick seg1 (speaker_0001), not the shorter seg2.
  boundary.reconciled_words.push_back({"word_a", 1000000, 3500000, 0.9, 0});
  boundary.reconciled_word_count = 1;

  svp::audio::SpeakerSegment seg1;
  seg1.id = "speakerseg_000000";
  seg1.speaker_id = "speaker_0001";
  seg1.timing = {0, 4000000};
  boundary.speaker_segments.push_back(std::move(seg1));

  svp::audio::SpeakerSegment seg2;
  seg2.id = "speakerseg_000001";
  seg2.speaker_id = "speaker_0002";
  seg2.timing = {3000000, 5000000};
  boundary.speaker_segments.push_back(std::move(seg2));

  (void)svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/words.jsonl");
  std::string line;
  std::getline(input, line);
  const nlohmann::json word = nlohmann::json::parse(line);
  assert(word["speaker_id"] == "speaker_0001");

  std::filesystem::remove_all(root);
}

void test_word_assignment_expands_sustained_non_dominant_utterance() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-word-assign-utterance-test";
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

  for (int i = 0; i < 12; ++i) {
    std::string text = "word_" + std::to_string(i);
    if (i == 11) text += ".";
    boundary.reconciled_words.push_back({
        text,
        static_cast<std::int64_t>(i * 500000),
        static_cast<std::int64_t>((i + 1) * 500000),
        0.9,
        0});
  }
  boundary.reconciled_word_count = boundary.reconciled_words.size();

  svp::audio::SpeakerSegment dominant_before;
  dominant_before.id = "speakerseg_000000";
  dominant_before.speaker_id = "speaker_0001";
  dominant_before.timing = {0, 1000000};
  boundary.speaker_segments.push_back(std::move(dominant_before));

  svp::audio::SpeakerSegment minority_island;
  minority_island.id = "speakerseg_000001";
  minority_island.speaker_id = "speaker_0002";
  minority_island.timing = {1000000, 5000000};
  boundary.speaker_segments.push_back(std::move(minority_island));

  svp::audio::SpeakerSegment dominant_after;
  dominant_after.id = "speakerseg_000002";
  dominant_after.speaker_id = "speaker_0001";
  dominant_after.timing = {5000000, 12000000};
  boundary.speaker_segments.push_back(std::move(dominant_after));

  const svp::audio::TranscriptWriteResult result =
      svp::audio::write_transcript_artifacts(boundary, root);
  assert(result.word_count == 12);
  assert(result.speaker_count == 2);

  std::ifstream input(root / "transcript/words.jsonl");
  std::string line;
  std::size_t word_count = 0;
  while (std::getline(input, line)) {
    const nlohmann::json word = nlohmann::json::parse(line);
    assert(word["speaker_id"] == "speaker_0002");
    ++word_count;
  }
  assert(word_count == 12);

  std::ifstream transcript_input(root / "transcript/transcript.json");
  const nlohmann::json transcript = nlohmann::json::parse(transcript_input);
  assert(transcript["speaker_count"] == 2);

  std::filesystem::remove_all(root);
}

void test_word_assignment_gap_nearest_within_tolerance() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-word-assign-gap-tolerance-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.one_speaker_mode = false;
  boundary.diarization_status = "ran";
  boundary.speaker_count = 1;

  // Word [3.2s, 3.5s) has no overlap with seg [0, 3s).
  // Word start to seg end = 200000 us (within 500000 tolerance).
  boundary.reconciled_words.push_back({"word_gap", 3200000, 3500000, 0.9, 0});
  boundary.reconciled_word_count = 1;

  svp::audio::SpeakerSegment seg1;
  seg1.id = "speakerseg_000000";
  seg1.speaker_id = "speaker_0001";
  seg1.timing = {0, 3000000};
  boundary.speaker_segments.push_back(std::move(seg1));

  (void)svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/words.jsonl");
  std::string line;
  std::getline(input, line);
  const nlohmann::json word = nlohmann::json::parse(line);
  assert(word["speaker_id"] == "speaker_0001");

  std::filesystem::remove_all(root);
}

void test_word_assignment_single_speaker_gap_beyond_tolerance_uses_sole_speaker() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-word-assign-gap-beyond-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.one_speaker_mode = false;
  boundary.diarization_status = "ran";
  boundary.speaker_count = 1;

  // Word [4s, 4.3s) has no overlap with seg [0, 3s).
  // Word start to seg end = 1000000 us (beyond 500000 tolerance).
  boundary.reconciled_words.push_back({"word_far", 4000000, 4300000, 0.9, 0});
  boundary.reconciled_word_count = 1;

  svp::audio::SpeakerSegment seg1;
  seg1.id = "speakerseg_000000";
  seg1.speaker_id = "speaker_0001";
  seg1.timing = {0, 3000000};
  boundary.speaker_segments.push_back(std::move(seg1));

  (void)svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/words.jsonl");
  std::string line;
  std::getline(input, line);
  const nlohmann::json word = nlohmann::json::parse(line);
  assert(word["speaker_id"] == "speaker_0001");

  std::ifstream sinput(root / "transcript/speakers.jsonl");
  bool found_single = false;
  while (std::getline(sinput, line)) {
    const nlohmann::json speaker = nlohmann::json::parse(line);
    if (speaker["id"] == "speaker_0001") {
      found_single = true;
      assert(speaker["total_speech_us"] == 300000);
    }
  }
  assert(found_single);

  std::filesystem::remove_all(root);
}

void test_word_assignment_no_segments_all_unknown() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-word-assign-no-segs-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  boundary.asr_status = svp::audio::AsrStatus::ran;
  boundary.one_speaker_mode = false;
  boundary.diarization_status = "ran";
  boundary.speaker_count = 0;

  boundary.reconciled_words.push_back({"word_a", 1000000, 2000000, 0.9, 0});
  boundary.reconciled_words.push_back({"word_b", 2000000, 3000000, 0.85, 0});
  boundary.reconciled_word_count = 2;

  (void)svp::audio::write_transcript_artifacts(boundary, root);

  std::ifstream input(root / "transcript/words.jsonl");
  std::string line;
  std::getline(input, line);
  const nlohmann::json word1 = nlohmann::json::parse(line);
  assert(word1["speaker_id"] == "speaker_unknown");

  std::getline(input, line);
  const nlohmann::json word2 = nlohmann::json::parse(line);
  assert(word2["speaker_id"] == "speaker_unknown");

  std::ifstream sinput(root / "transcript/speakers.jsonl");
  std::getline(sinput, line);
  const nlohmann::json speaker = nlohmann::json::parse(line);
  assert(speaker["id"] == "speaker_unknown");
  assert(speaker["total_speech_us"] == 2000000);

  std::filesystem::remove_all(root);
}

