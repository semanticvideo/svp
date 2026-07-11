#include "audio_test_support.hpp"

void test_real_sherpa_diarization_speaker_count_fixtures_when_enabled() {
  const char* model_dir_env = std::getenv("SVP_SHERPA_DIARIZATION_MODEL_DIR");
  if (!model_dir_env || std::string(model_dir_env).empty()) {
    return;
  }

  if (!svp::audio::is_sherpa_diarization_available()) {
    throw std::runtime_error("SVP_SHERPA_DIARIZATION_MODEL_DIR is set but sherpa-onnx is unavailable");
  }

  const std::filesystem::path fixture_root =
      std::filesystem::path(SVP_REPO_ROOT) / "fixtures/audio/sherpa-diarization";
  const std::vector<std::pair<std::string, int32_t>> cases = {
      {"one-speaker.wav", 1},
      {"two-speaker.wav", 2},
      {"similar-timbre-two-speaker.wav", 2},
      {"three-speaker.wav", 3},
      {"four-speaker.wav", 4},
  };

  for (const auto& [filename, expected_speakers] : cases) {
    const std::filesystem::path fixture = fixture_root / filename;
    if (!std::filesystem::exists(fixture)) {
      throw std::runtime_error("missing Sherpa diarization fixture: " +
                               fixture.string());
    }

    std::vector<std::pair<std::size_t, std::size_t>> progress;
    const svp::audio::SherpaDiarizationResult result =
        svp::audio::run_sherpa_diarization(
            fixture, model_dir_env, {},
            [&progress](std::size_t current, std::size_t total) {
              progress.emplace_back(current, total);
            });
    if (!result.ran) {
      throw std::runtime_error("Sherpa diarization did not run for fixture: " +
                               fixture.string());
    }
    if (result.final_speaker_count != expected_speakers) {
      throw std::runtime_error(
          "unexpected speaker count for " + filename + ": got " +
          std::to_string(result.final_speaker_count) + ", expected " +
          std::to_string(expected_speakers));
    }
    if (result.segments.empty()) {
      throw std::runtime_error("Sherpa diarization produced no segments for fixture: " +
                               fixture.string());
    }
    if (progress.empty() || progress.front().first != 0 ||
        progress.back().first != progress.back().second) {
      throw std::runtime_error(
          "Sherpa diarization progress did not span the complete fixture: " +
          fixture.string());
    }
    if (progress.front().second !=
        svp::audio::diarization_chunk_count(fixture)) {
      throw std::runtime_error(
          "Sherpa diarization progress total did not match its chunk plan: " +
          fixture.string());
    }
    for (std::size_t index = 1; index < progress.size(); ++index) {
      if (progress[index].first < progress[index - 1].first ||
          progress[index].second != progress.front().second) {
        throw std::runtime_error(
            "Sherpa diarization progress was not monotonic for fixture: " +
            fixture.string());
      }
    }
  }
}

struct ExpectedSpeechBlock {
  std::int64_t start_us;
  std::int64_t end_us;
  std::string logical_speaker;
};

struct RealAsrFixtureCase {
  std::string filename;
  std::int64_t duration_us;
  std::vector<ExpectedSpeechBlock> blocks;
  bool require_uniform_block_attribution = false;
};

void assert_real_asr_fixture_word_attribution(
    const RealAsrFixtureCase& fixture_case,
    const std::filesystem::path& fixture_root,
    const std::filesystem::path& model_cache_root) {
  const std::filesystem::path source_wav = fixture_root / fixture_case.filename;
  if (!std::filesystem::exists(source_wav)) {
    throw std::runtime_error("missing real ASR diarization fixture: " +
                             source_wav.string());
  }

  const std::filesystem::path staging_root =
      std::filesystem::temp_directory_path() /
      ("svp-real-asr-diar-fixture-" + fixture_case.filename);
  std::filesystem::remove_all(staging_root);
  std::filesystem::create_directories(staging_root / "media/audio");
  std::filesystem::copy_file(
      source_wav,
      staging_root / "media/audio/analysis_mono_16k.wav",
      std::filesystem::copy_options::overwrite_existing);

  const svp::audio::AsrChunkPlanResult chunk_plan =
      svp::audio::build_asr_chunk_plan(fixture_case.duration_us);
  svp::audio::AsrExecutionBoundary asr_boundary =
      svp::audio::build_asr_execution_boundary(
          chunk_plan,
          true,
          svp::audio::is_whisper_runtime_available(),
          svp::audio::check_asr_model_in_cache(
              "model_whisper_small_en", model_cache_root),
          svp::audio::verify_asr_model_files(
              "model_whisper_small_en", model_cache_root));
  asr_boundary = svp::audio::execute_asr_boundary(
      std::move(asr_boundary), staging_root, model_cache_root);
  if (asr_boundary.asr_status != svp::audio::AsrStatus::ran) {
    throw std::runtime_error("real ASR did not run for fixture: " +
                             fixture_case.filename);
  }
  if (asr_boundary.reconciled_words.empty()) {
    throw std::runtime_error("real ASR produced no words for fixture: " +
                             fixture_case.filename);
  }

  if (!svp::audio::is_sherpa_diarization_available()) {
    throw std::runtime_error(
        "real ASR attribution fixture requested but sherpa-onnx is unavailable");
  }

  svp::audio::DiarizationExecutionBoundary diar_boundary =
      svp::audio::build_diarization_boundary(
          true,
          true,
          svp::audio::check_diarization_model_in_cache(
              "model_sherpa_onnx_diarization", model_cache_root),
          svp::audio::verify_diarization_model_files(
              "model_sherpa_onnx_diarization", model_cache_root),
          fixture_case.duration_us);
  diar_boundary = svp::audio::execute_diarization_boundary(
      std::move(diar_boundary),
      staging_root,
      model_cache_root,
      false,
      false,
      asr_boundary.reconciled_words);

  if (diar_boundary.diarization_status != svp::audio::DiarizationStatus::ran) {
    throw std::runtime_error("real diarization did not run for fixture: " +
                             fixture_case.filename);
  }
  svp::audio::AsrExecutionBoundary asr_with_diar = asr_boundary;
  asr_with_diar.diarization_status =
      svp::audio::diarization_status_to_string(diar_boundary.diarization_status);
  asr_with_diar.one_speaker_mode = false;
  asr_with_diar.speaker_count = diar_boundary.speaker_count;
  asr_with_diar.speaker_segments = diar_boundary.speaker_segments;
  asr_with_diar.word_speaker_assignments = diar_boundary.word_speaker_assignments;
  const svp::audio::TranscriptWriteResult transcript_result =
      svp::audio::write_transcript_artifacts(asr_with_diar, staging_root);
  if (!transcript_result.words_written) {
    throw std::runtime_error("transcript words were not written for fixture: " +
                             fixture_case.filename);
  }

  std::vector<std::string> final_word_speaker_ids;
  {
    std::ifstream input(staging_root / "transcript/words.jsonl");
    std::string line;
    while (std::getline(input, line)) {
      const nlohmann::json word = nlohmann::json::parse(line);
      final_word_speaker_ids.push_back(word.at("speaker_id").get<std::string>());
    }
  }
  if (final_word_speaker_ids.size() != asr_boundary.reconciled_words.size()) {
    throw std::runtime_error("final transcript word count mismatch for fixture: " +
                             fixture_case.filename);
  }

  std::map<std::string, std::string> logical_to_actual;
  std::size_t checked_words = 0;
  for (const ExpectedSpeechBlock& block : fixture_case.blocks) {
    std::map<std::string, std::size_t> speaker_counts;
    for (std::size_t i = 0; i < asr_boundary.reconciled_words.size(); ++i) {
      const svp::audio::AsrWord& word = asr_boundary.reconciled_words[i];
      const std::int64_t midpoint_us = word.start_us + ((word.end_us - word.start_us) / 2);
      const bool inside_block = fixture_case.require_uniform_block_attribution
                                    ? word.start_us >= block.start_us &&
                                          word.end_us <= block.end_us
                                    : midpoint_us >= block.start_us &&
                                          midpoint_us < block.end_us;
      if (inside_block) {
        ++speaker_counts[final_word_speaker_ids[i]];
      }
    }
    if (speaker_counts.empty()) {
      throw std::runtime_error("no ASR words landed in expected block for fixture: " +
                               fixture_case.filename);
    }
    if (fixture_case.require_uniform_block_attribution &&
        speaker_counts.size() != 1) {
      throw std::runtime_error(
          "fixture speaker block contains mixed attribution for " +
          fixture_case.filename);
    }

    const auto majority =
        std::max_element(speaker_counts.begin(), speaker_counts.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return lhs.second < rhs.second;
                         });
    checked_words += majority->second;
    const auto existing = logical_to_actual.find(block.logical_speaker);
    if (existing == logical_to_actual.end()) {
      for (const auto& [other_logical, actual] : logical_to_actual) {
        if (actual == majority->first) {
          throw std::runtime_error("fixture logical speakers collapsed together for " +
                                   fixture_case.filename);
        }
      }
      logical_to_actual.emplace(block.logical_speaker, majority->first);
    } else if (existing->second != majority->first) {
      throw std::runtime_error("fixture logical speaker changed assignment for " +
                               fixture_case.filename);
    }
  }
  if (checked_words == 0) {
    throw std::runtime_error("no fixture words were checked for " +
                             fixture_case.filename);
  }

  std::filesystem::remove_all(staging_root);
}

void test_real_asr_diarization_word_attribution_fixtures_when_enabled() {
  const char* model_cache_env = std::getenv("SVP_MODEL_CACHE_DIR");
  if (!model_cache_env || std::string(model_cache_env).empty()) {
    return;
  }

  const std::filesystem::path model_cache_root = model_cache_env;
  const std::filesystem::path fixture_root =
      std::filesystem::path(SVP_REPO_ROOT) / "fixtures/audio/sherpa-diarization";
  const std::vector<RealAsrFixtureCase> cases = {
      {"one-speaker.wav",
       45000000,
       {{0, 45000000, "A"}}},
      {"two-speaker.wav",
       58000000,
       {{0, 10000000, "A"},
        {16000000, 26000000, "B"},
        {32000000, 42000000, "A"},
        {48000000, 58000000, "B"}}},
      {"three-speaker.wav",
       90000000,
       {{0, 10000000, "A"},
        {16000000, 26000000, "B"},
        {32000000, 42000000, "C"},
        {48000000, 58000000, "A"},
        {64000000, 74000000, "B"},
        {80000000, 90000000, "C"}}},
      {"similar-timbre-two-speaker.wav",
       16800000,
       {{240000, 2820000, "A"},
        {3030000, 6040000, "B"},
        {6120000, 11860000, "A"},
        {11940000, 13060000, "B"},
        {13140000, 14510000, "A"},
        {14590000, 16590000, "B"}},
       true},
  };

  for (const RealAsrFixtureCase& fixture_case : cases) {
    assert_real_asr_fixture_word_attribution(
        fixture_case, fixture_root, model_cache_root);
  }
}
