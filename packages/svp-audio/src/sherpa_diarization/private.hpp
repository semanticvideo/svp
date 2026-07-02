#pragma once

#include "svp/audio/sherpa_diarization.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace svp::audio::sherpa_diarization_internal {

inline constexpr int32_t kDiarizationSampleRate = 16000;
inline constexpr int32_t kMaxDiarizationChunkSamples = kDiarizationSampleRate * 5;
inline constexpr std::int64_t kUtteranceGapThresholdUs = 750000;
inline constexpr std::int64_t kUtterancePaddingUs = 150000;
inline constexpr float kSameSpeakerSimilarityThreshold = 0.60f;
inline constexpr float kGlobalSpeakerObservationMinGap = 0.10f;
inline constexpr float kGlobalSpeakerObservationFloorSimilarity = 0.14f;
inline constexpr int32_t kDominantStitchMinFinalSpeakers = 8;
inline constexpr std::size_t kDominantStitchMinObservations = 8;
inline constexpr float kDominantStitchCombinedSpeechShare = 0.70f;
inline constexpr float kDominantStitchMinTrackSpeechShare = 0.15f;
inline constexpr float kDominantStitchMaxOverlapShare = 0.01f;
inline constexpr float kSingleDominantCollapseSpeechShare = 0.95f;
inline constexpr float kSherpaLocalClusteringThreshold = 0.90f;
inline constexpr int64_t kDiarizationWindowSamples =
    static_cast<int64_t>(kDiarizationSampleRate) * 300;
inline constexpr int64_t kDiarizationWindowOverlapSamples =
    static_cast<int64_t>(kDiarizationSampleRate) * 2;
inline constexpr float kMinClippedSegmentDuration = 0.1f;
inline constexpr float kLocalSpeakerAssignmentMaxGapSec = 5.0f;
inline constexpr int32_t kMaxSpeakerEmbeddingSamples = kDiarizationSampleRate * 10;
inline constexpr int32_t kMinSpeakerEmbeddingSamples = kDiarizationSampleRate / 10;

struct SherpaOnnxOfflineSpeakerDiarizationSegment {
  float start;
  float end;
  int32_t speaker;
};

struct SherpaOnnxOfflineSpeakerSegmentationPyannoteModelConfig {
  const char* model;
};

struct SherpaOnnxOfflineSpeakerSegmentationModelConfig {
  SherpaOnnxOfflineSpeakerSegmentationPyannoteModelConfig pyannote;
  int32_t num_threads;
  int32_t debug;
  const char* provider;
};

struct SherpaOnnxSpeakerEmbeddingExtractorConfig {
  const char* model;
  int32_t num_threads;
  int32_t debug;
  const char* provider;
};

struct SherpaOnnxFastClusteringConfig {
  int32_t num_clusters;
  float threshold;
};

struct SherpaOnnxOfflineSpeakerDiarizationConfig {
  SherpaOnnxOfflineSpeakerSegmentationModelConfig segmentation;
  SherpaOnnxSpeakerEmbeddingExtractorConfig embedding;
  SherpaOnnxFastClusteringConfig clustering;
  float min_duration_on;
  float min_duration_off;
};

struct SherpaOnnxOnlineStream;

using CreateDiarizationFn = const void* (*)(const SherpaOnnxOfflineSpeakerDiarizationConfig*);
using DestroyDiarizationFn = void (*)(const void*);
using ProcessFn = const void* (*)(const void*, const float*, int32_t);
using DestroyResultFn = void (*)(const void*);
using GetNumSpeakersFn = int32_t (*)(const void*);
using GetNumSegmentsFn = int32_t (*)(const void*);
using SortByStartTimeFn = const SherpaOnnxOfflineSpeakerDiarizationSegment* (*)(const void*);
using DestroySegmentFn = void (*)(const SherpaOnnxOfflineSpeakerDiarizationSegment*);

using CreateEmbeddingExtractorFn = const void* (*)(const SherpaOnnxSpeakerEmbeddingExtractorConfig*);
using DestroyEmbeddingExtractorFn = void (*)(const void*);
using EmbeddingDimFn = int32_t (*)(const void*);
using EmbeddingCreateStreamFn = const SherpaOnnxOnlineStream* (*)(const void*);
using OnlineStreamAcceptWaveformFn = void (*)(const SherpaOnnxOnlineStream*, int32_t, const float*, int32_t);
using OnlineStreamInputFinishedFn = void (*)(const SherpaOnnxOnlineStream*);
using EmbeddingIsReadyFn = int32_t (*)(const void*, const SherpaOnnxOnlineStream*);
using EmbeddingComputeFn = const float* (*)(const void*, const SherpaOnnxOnlineStream*);
using EmbeddingDestroyFn = void (*)(const float*);
using DestroyOnlineStreamFn = void (*)(const SherpaOnnxOnlineStream*);

struct SherpaDiarizationApi {
  void* lib_handle = nullptr;
  CreateDiarizationFn create = nullptr;
  DestroyDiarizationFn destroy = nullptr;
  ProcessFn process = nullptr;
  DestroyResultFn destroy_result = nullptr;
  GetNumSpeakersFn get_num_speakers = nullptr;
  GetNumSegmentsFn get_num_segments = nullptr;
  SortByStartTimeFn sort_by_start_time = nullptr;
  DestroySegmentFn destroy_segment = nullptr;

  CreateEmbeddingExtractorFn emb_create = nullptr;
  DestroyEmbeddingExtractorFn emb_destroy = nullptr;
  EmbeddingDimFn emb_dim = nullptr;
  EmbeddingCreateStreamFn emb_create_stream = nullptr;
  OnlineStreamAcceptWaveformFn stream_accept = nullptr;
  OnlineStreamInputFinishedFn stream_input_finished = nullptr;
  EmbeddingIsReadyFn emb_is_ready = nullptr;
  EmbeddingComputeFn emb_compute = nullptr;
  EmbeddingDestroyFn emb_destroy_vec = nullptr;
  DestroyOnlineStreamFn stream_destroy = nullptr;

  bool loaded = false;
};

struct SherpaLibState {
  std::string explicit_path;
  std::string loaded_path;
  std::vector<std::string> attempted_paths;
};

struct PcmS16MonoWavInfo {
  std::filesystem::path path;
  std::streamoff data_offset = 0;
  std::size_t sample_count = 0;
};

struct DiarizationWindowRange {
  std::size_t accepted_start;
  std::size_t accepted_end;
  std::size_t process_start;
  std::size_t process_end;
};

struct LocalSpeakerAssignment {
  int32_t global_speaker = -1;
  float last_end_sec = 0.0f;
};

struct SpeakerObservation {
  int32_t observation_id = 0;
  float first_start_sec = 0.0f;
  std::vector<float> embedding;
};

SherpaLibState& lib_state();
SherpaDiarizationApi& get_api();

std::vector<float> read_pcm_s16le_mono_wav_samples(const std::filesystem::path& path);
PcmS16MonoWavInfo read_pcm_s16le_mono_wav_info(const std::filesystem::path& path);
std::vector<float> read_pcm_s16le_mono_wav_range(
    const PcmS16MonoWavInfo& info,
    std::size_t start_sample,
    std::size_t end_sample);

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);
void normalize_embedding(std::vector<float>& v);
bool has_embedding_signal(const std::vector<float>& embedding);
std::vector<float> compute_bounded_speaker_embedding(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& all_samples,
    std::size_t sample_base,
    const std::vector<SherpaDiarizationSegment>& speaker_segments);

std::vector<DiarizationWindowRange> build_diarization_windows(std::size_t sample_count);
bool clip_segment_to_range(SherpaDiarizationSegment& seg,
                           float accepted_start_sec,
                           float accepted_end_sec);

std::map<int32_t, int32_t> cluster_speaker_observations(
    const std::vector<SpeakerObservation>& observations,
    const std::set<std::pair<int32_t, int32_t>>& cannot_link_observations);
void stitch_dominant_non_overlapping_tracks(
    std::vector<SherpaDiarizationSegment>& segments,
    int32_t& final_speaker_count,
    std::size_t observation_count);
void collapse_single_dominant_track(std::vector<SherpaDiarizationSegment>& segments,
                                    int32_t& final_speaker_count);

bool ends_utterance(const std::string& text);

}  // namespace svp::audio::sherpa_diarization_internal
