#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

struct AsrChunkPlan {
  std::string chunk_id;
  std::int64_t source_start_us = 0;
  std::int64_t source_end_us = 0;
  std::int64_t overlap_before_us = 0;
  std::int64_t overlap_after_us = 0;
  std::string input_ref;
  std::string output_ref;
  std::string model_id;
  std::string runtime;
  std::string asr_status;
};

struct AsrChunkPlanResult {
  std::vector<AsrChunkPlan> chunks;
  std::int64_t chunk_duration_us = 0;
  std::int64_t overlap_us = 0;
  std::int64_t total_duration_us = 0;
  std::vector<std::string> blockers;
};

inline constexpr std::int64_t kDefaultAsrChunkDurationUs = 15000000;
inline constexpr std::int64_t kDefaultAsrChunkOverlapUs = 2000000;

[[nodiscard]] AsrChunkPlanResult build_asr_chunk_plan(
    std::int64_t total_duration_us,
    std::int64_t chunk_duration_us = kDefaultAsrChunkDurationUs,
    std::int64_t overlap_us = kDefaultAsrChunkOverlapUs,
    const std::string& input_ref = "media/audio/analysis_mono_16k.wav",
    const std::string& model_id = "model_whisper_small_en",
    const std::string& runtime = "whisper.cpp");

[[nodiscard]] nlohmann::json asr_chunk_plan_to_json(const AsrChunkPlanResult& plan);

[[nodiscard]] nlohmann::json asr_chunk_to_json(const AsrChunkPlan& chunk);

struct AsrWord {
  std::string text;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  double confidence = 0.0;
  std::int64_t chunk_ordinal = 0;
};

[[nodiscard]] std::vector<AsrWord> reconcile_overlapping_chunks(
    const std::vector<std::vector<AsrWord>>& chunk_words,
    const std::vector<AsrChunkPlan>& chunks);

}  // namespace svp::audio
