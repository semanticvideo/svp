#pragma once

#include "svp/audio/asr_execution_boundary.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace svp::audio {

struct TranscriptWriteResult {
  bool transcript_written = false;
  bool words_written = false;
  bool speakers_written = false;
  bool chunk_provenance_written = false;
  bool speech_regions_written = false;
  bool speaker_segments_written = false;
  std::size_t word_count = 0;
  std::size_t speaker_count = 0;
  std::size_t speech_region_count = 0;
  std::string transcript_status;
  std::vector<std::string> blockers;
};

[[nodiscard]] TranscriptWriteResult write_transcript_artifacts(
    const AsrExecutionBoundary& boundary,
    const std::filesystem::path& staging_root);

[[nodiscard]] nlohmann::json transcript_write_result_to_json(
    const TranscriptWriteResult& result);

}  // namespace svp::audio
