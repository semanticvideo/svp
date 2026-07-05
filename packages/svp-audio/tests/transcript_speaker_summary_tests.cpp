#include "audio_test_support.hpp"

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

  (void)svp::audio::write_transcript_artifacts(boundary, root);

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

  const svp::audio::TranscriptWriteResult result =
      svp::audio::write_transcript_artifacts(boundary, root);
  assert(result.word_count == 12);
  assert(result.speaker_count == 2);

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

  (void)svp::audio::write_transcript_artifacts(boundary, root);

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

