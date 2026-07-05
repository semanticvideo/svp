#pragma once

#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/transcript_writer.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace svp::audio::transcript_writer_internal {

struct WordRecordBuildResult {
  std::vector<nlohmann::json> records;
  std::map<std::string, std::vector<TimeSpan>> speaker_intervals;
};

std::string asr_status_string(AsrStatus status);

nlohmann::json blocked_transcript_json(const AsrExecutionBoundary& boundary);

nlohmann::json ran_transcript_json(const AsrExecutionBoundary& boundary,
                                   std::size_t word_count,
                                   std::size_t speaker_count);

nlohmann::json chunk_provenance_json(const AsrChunkPlan& chunk,
                                     const std::string& processor_id,
                                     const std::string& asr_status,
                                     const std::string& diarization_status);

std::vector<std::string> assign_word_speakers(
    const AsrExecutionBoundary& boundary);

WordRecordBuildResult build_word_records(
    const std::vector<AsrWord>& words,
    const std::vector<std::string>& speaker_assignments);

std::vector<nlohmann::json> build_speaker_records(
    const AsrExecutionBoundary& boundary,
    const std::map<std::string, std::vector<TimeSpan>>& speaker_intervals);

}  // namespace svp::audio::transcript_writer_internal
