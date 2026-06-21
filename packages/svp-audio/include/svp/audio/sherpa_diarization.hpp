#pragma once

#include "svp/audio/transcript_records.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace svp::audio {

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
  std::vector<std::vector<float>> pairwise_similarity_matrix;
  std::vector<ClusterMergeDecision> merge_decisions;
  std::string reconciliation_method;
  std::vector<std::string> blockers;
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
    const std::filesystem::path& model_dir);

[[nodiscard]] bool is_sherpa_diarization_available();

}  // namespace svp::audio
