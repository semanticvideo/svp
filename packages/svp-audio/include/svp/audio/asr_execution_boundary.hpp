#pragma once

#include "svp/models/reference_processor_model_ids.hpp"

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/transcript_records.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace svp::audio {

enum class AsrStatus {
  planned,
  blocked,
  ran,
};

[[nodiscard]] std::string asr_status_to_string(AsrStatus status);

struct AsrExecutionBoundary {
  std::string processor_id = "proc_whisper_asr_0001";
  std::string model_id = svp::models::kWhisperSmallEnglishModelId;
  std::string runtime = "whisper.cpp";
  std::string execution_provider = "cpu";
  AsrChunkPlanResult chunk_plan;
  std::vector<std::string> input_refs;
  std::vector<std::string> staged_chunk_output_refs;
  std::string transcript_output_ref = "transcript/transcript.json";
  std::string words_output_ref = "transcript/words.jsonl";
  std::string speakers_output_ref = "transcript/speakers.jsonl";
  std::string chunk_provenance_ref = "transcript/asr_chunk_provenance.jsonl";
  std::vector<std::string> blockers;
  bool analysis_audio_available = false;
  bool model_runtime_available = false;
  bool model_available = false;
  bool model_verified = false;
  AsrStatus asr_status = AsrStatus::planned;
  bool transcript_written = false;
  bool words_written = false;
  bool speakers_written = false;
  bool chunk_provenance_written = false;
  std::size_t raw_word_count = 0;
  std::size_t reconciled_word_count = 0;
  std::size_t speaker_count = 0;
  bool one_speaker_mode = true;
  std::string diarization_status = "unavailable";
  std::string diarization_processor_id = "proc_sherpa_diar_0001";
  std::string diarization_note;
  std::vector<std::string> diarization_blockers;
  std::vector<SpeakerSegment> speaker_segments;
  std::vector<AsrWord> reconciled_words;
  std::vector<std::string> word_speaker_assignments;
  std::vector<std::string> speaker_source_audio_stream_ids;
  std::vector<std::vector<std::string>> speaker_source_audio_stream_groups;
};

[[nodiscard]] bool check_asr_model_in_cache(
    const std::string& model_id,
    const std::filesystem::path& model_cache_root);

[[nodiscard]] bool verify_asr_model_files(
    const std::string& model_id,
    const std::filesystem::path& model_cache_root);

[[nodiscard]] AsrExecutionBoundary build_asr_execution_boundary(
    const AsrChunkPlanResult& chunk_plan,
    bool analysis_audio_available,
    bool model_runtime_available,
    bool model_available,
    bool model_verified,
    const std::string& analysis_audio_ref = "media/audio/analysis_mono_16k.wav");

using AsrChunkProgressCallback =
    std::function<void(std::size_t current, std::size_t total)>;

[[nodiscard]] AsrExecutionBoundary execute_asr_boundary(
    AsrExecutionBoundary boundary,
    const std::filesystem::path& staging_root,
    const std::filesystem::path& model_cache_root,
    AsrChunkProgressCallback on_chunk_progress = {});

[[nodiscard]] nlohmann::json asr_execution_boundary_to_json(
    const AsrExecutionBoundary& boundary);

}  // namespace svp::audio
