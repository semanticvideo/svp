#pragma once

#include "svp/audio/asr_chunk_planner.hpp"

#include <cstddef>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace svp::audio {

enum class AsrStatus {
  planned,
  blocked,
  ran,
};

struct AsrExecutionBoundary {
  std::string processor_id = "proc_whisper_asr_0001";
  std::string model_id = "model_whisper_large_v3_turbo_q5_0";
  std::string runtime = "onnxruntime";
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
    bool model_verified);

[[nodiscard]] AsrExecutionBoundary execute_asr_boundary(
    AsrExecutionBoundary boundary,
    const std::filesystem::path& staging_root,
    const std::filesystem::path& model_cache_root);

[[nodiscard]] nlohmann::json asr_execution_boundary_to_json(
    const AsrExecutionBoundary& boundary);

}  // namespace svp::audio
