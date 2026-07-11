#pragma once

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/whisper_token_table.hpp"

#include <cstdint>
#include <vector>

namespace svp::audio {

[[nodiscard]] std::vector<AsrWord> decode_untimestamped_whisper_words(
    const std::vector<int>& token_ids,
    const std::vector<double>& token_probabilities,
    const WhisperTokenTable& token_table,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us,
    int timestamp_begin);

}  // namespace svp::audio
