#pragma once

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/transcript_records.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace svp::audio {

using DiarizationProgressCallback =
    std::function<void(std::size_t current, std::size_t total)>;

struct SherpaDiarizationSegment {
  float start_sec = 0.0f;
  float end_sec = 0.0f;
  int32_t speaker_id = 0;
};

struct ClusterMergeDecision {
  int32_t cluster_a = 0;
  int32_t cluster_b = 0;
  float cosine_similarity = 0.0f;
  bool merged = false;
};

struct SherpaDiarizationResult {
  bool ran = false;
  int32_t preliminary_cluster_count = 0;
  int32_t final_speaker_count = 0;
  std::vector<SherpaDiarizationSegment> segments;
  std::vector<SherpaDiarizationSegment> preliminary_segments;
  std::vector<std::vector<float>> pairwise_similarity_matrix;
  std::vector<ClusterMergeDecision> merge_decisions;
  std::string reconciliation_method;
  std::vector<std::string> blockers;
  std::vector<std::vector<float>> cluster_centroids;
  std::vector<int32_t> cluster_ids_for_centroids;
  std::map<int32_t, int32_t> cluster_to_final;
  std::vector<std::vector<float>> final_speaker_fingerprints;
  std::vector<std::vector<float>> segment_fingerprint_similarities;
  std::vector<std::string> word_speaker_assignments;
};

struct ReconciliationResult {
  std::vector<ClusterMergeDecision> merge_decisions;
  std::map<int32_t, int32_t> cluster_to_final;
  int32_t final_speaker_count = 0;
  std::string method_description;
};

[[nodiscard]] ReconciliationResult reconcile_clusters(
    const std::vector<std::vector<float>>& similarity_matrix,
    const std::vector<int32_t>& cluster_ids);

[[nodiscard]] SherpaDiarizationResult run_sherpa_diarization(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::vector<AsrWord>& words = {},
    DiarizationProgressCallback on_progress = {});

[[nodiscard]] std::vector<std::string> refine_word_speakers_by_embedding(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result);

[[nodiscard]] std::vector<std::string> replay_word_speaker_assignments(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result);

[[nodiscard]] bool is_sherpa_diarization_available();

void set_sherpa_lib_path(const std::string& path);

[[nodiscard]] std::string sherpa_lib_path_used();

[[nodiscard]] std::vector<std::string> sherpa_lib_paths_attempted();

}  // namespace svp::audio
