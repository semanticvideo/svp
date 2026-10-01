#pragma once

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/transcript_records.hpp"
#include "svp/models/thread_plan.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace svp::audio {

using DiarizationProgressCallback =
    std::function<void(std::size_t current, std::size_t total)>;

[[nodiscard]] std::size_t diarization_chunk_count(
    const std::filesystem::path& wav_path);

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

// `threads` is the ThreadPlan sherpa entry; every sherpa-onnx session the
// call creates uses it.
[[nodiscard]] SherpaDiarizationResult run_sherpa_diarization(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const svp::models::SherpaThreadCounts& threads,
    const std::vector<AsrWord>& words = {},
    DiarizationProgressCallback on_progress = {});

[[nodiscard]] std::vector<std::string> refine_word_speakers_by_embedding(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const svp::models::SherpaThreadCounts& threads,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result);

[[nodiscard]] std::vector<std::string> replay_word_speaker_assignments(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const svp::models::SherpaThreadCounts& threads,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result);

[[nodiscard]] bool is_sherpa_diarization_available();

void set_sherpa_lib_path(const std::string& path);

// Where the loaded sherpa-onnx C API library came from, in search order:
// the explicit path, SHERPA_ONNX_LIB_PATH, the installed runtime bundle, then
// the legacy pip/venv/Homebrew/system locations. `none` until a library loads.
enum class SherpaLibSource { none, explicit_path, environment, bundled, legacy_search };

// The pinned library of an installed SVP runtime bundle. Searched after the
// explicit path and SHERPA_ONNX_LIB_PATH and before every legacy location.
// Must be set before the library is first loaded.
void set_sherpa_bundled_lib_path(const std::string& path);

[[nodiscard]] SherpaLibSource sherpa_lib_source_used();

[[nodiscard]] std::string_view sherpa_lib_source_name(SherpaLibSource source);

[[nodiscard]] std::string sherpa_lib_path_used();

[[nodiscard]] std::vector<std::string> sherpa_lib_paths_attempted();

}  // namespace svp::audio
