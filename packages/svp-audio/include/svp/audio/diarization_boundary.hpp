#pragma once

#include "svp/models/reference_processor_model_ids.hpp"

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/transcript_records.hpp"

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::audio {

enum class DiarizationStatus {
  planned,
  unavailable,
  ran,
  fallback_one_speaker,
  user_declared_single_speaker,
};

struct DiarizationExecutionBoundary {
  std::string processor_id = "proc_sherpa_diar_0001";
  std::string model_id = svp::models::kSherpaOnnxDiarizationModelId;
  std::string runtime = "onnxruntime";
  std::string execution_provider = "cpu";
  std::string speaker_segments_output_ref = "transcript/speaker_segments.jsonl";
  std::vector<std::string> blockers;
  bool analysis_audio_available = false;
  bool model_runtime_available = false;
  bool model_available = false;
  bool model_verified = false;
  DiarizationStatus diarization_status = DiarizationStatus::planned;
  std::size_t speaker_count = 0;
  std::int64_t total_duration_us = 0;
  std::vector<SpeakerSegment> speaker_segments;
  std::string reconciliation_method;
  int32_t preliminary_cluster_count = 0;
  nlohmann::json pairwise_similarity_matrix_json = nullptr;
  nlohmann::json merge_decisions_json = nullptr;
  SherpaDiarizationResult raw_diar_result;
  std::vector<std::string> word_speaker_assignments;
};

[[nodiscard]] bool check_diarization_model_in_cache(
    const std::string& model_id,
    const std::filesystem::path& model_cache_root);

[[nodiscard]] bool verify_diarization_model_files(
    const std::string& model_id,
    const std::filesystem::path& model_cache_root);

[[nodiscard]] DiarizationExecutionBoundary build_diarization_boundary(
    bool analysis_audio_available,
    bool model_runtime_available,
    bool model_available,
    bool model_verified,
    std::int64_t total_duration_us);

[[nodiscard]] DiarizationExecutionBoundary execute_diarization_boundary(
    DiarizationExecutionBoundary boundary,
    const std::filesystem::path& staging_root,
    const std::filesystem::path& model_cache_root,
    bool allow_fallback = false,
    bool force_single_speaker = false,
    const std::vector<AsrWord>& words = {},
    DiarizationProgressCallback on_progress = {});

[[nodiscard]] std::string diarization_status_to_string(DiarizationStatus status);

[[nodiscard]] nlohmann::json diarization_execution_boundary_to_json(
    const DiarizationExecutionBoundary& boundary);

}  // namespace svp::audio
