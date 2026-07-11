#pragma once

#include "svp/audio/asr_chunk_planner.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace svp::audio {

void set_whisper_verbose(bool verbose);

struct WhisperSegment {
  std::string text;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::vector<AsrWord> words;
};

struct WhisperInferenceResult {
  bool ran = false;
  std::vector<WhisperSegment> segments;
  std::vector<AsrWord> all_words;
  std::vector<int> decoded_token_ids;
  std::string termination_reason;
  std::vector<std::string> blockers;
};

[[nodiscard]] WhisperInferenceResult run_whisper_inference(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::string& chunk_id,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us);

[[nodiscard]] bool is_whisper_runtime_available();

}  // namespace svp::audio
