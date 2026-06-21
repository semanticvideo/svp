#include "svp/audio/sherpa_diarization.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <vector>

namespace svp::audio {
namespace {

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

SherpaDiarizationApi& get_api() {
  static SherpaDiarizationApi api;
  if (api.loaded) return api;
  api.loaded = true;

  const char* lib_path = std::getenv("SHERPA_ONNX_LIB_PATH");
  std::string path;
  if (lib_path && lib_path[0]) {
    path = lib_path;
  } else {
    path = "/Users/domesposito/Library/Python/3.9/lib/python/site-packages/sherpa_onnx/lib/libsherpa-onnx-c-api.dylib";
  }

  api.lib_handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!api.lib_handle) {
    return api;
  }

  auto load = [&](auto& fn, const char* name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(api.lib_handle, name));
  };

  load(api.create, "SherpaOnnxCreateOfflineSpeakerDiarization");
  load(api.destroy, "SherpaOnnxDestroyOfflineSpeakerDiarization");
  load(api.process, "SherpaOnnxOfflineSpeakerDiarizationProcess");
  load(api.destroy_result, "SherpaOnnxOfflineSpeakerDiarizationDestroyResult");
  load(api.get_num_speakers, "SherpaOnnxOfflineSpeakerDiarizationResultGetNumSpeakers");
  load(api.get_num_segments, "SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments");
  load(api.sort_by_start_time, "SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime");
  load(api.destroy_segment, "SherpaOnnxOfflineSpeakerDiarizationDestroySegment");

  load(api.emb_create, "SherpaOnnxCreateSpeakerEmbeddingExtractor");
  load(api.emb_destroy, "SherpaOnnxDestroySpeakerEmbeddingExtractor");
  load(api.emb_dim, "SherpaOnnxSpeakerEmbeddingExtractorDim");
  load(api.emb_create_stream, "SherpaOnnxSpeakerEmbeddingExtractorCreateStream");
  load(api.stream_accept, "SherpaOnnxOnlineStreamAcceptWaveform");
  load(api.stream_input_finished, "SherpaOnnxOnlineStreamInputFinished");
  load(api.emb_is_ready, "SherpaOnnxSpeakerEmbeddingExtractorIsReady");
  load(api.emb_compute, "SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding");
  load(api.emb_destroy_vec, "SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding");
  load(api.stream_destroy, "SherpaOnnxDestroyOnlineStream");

  if (!api.create || !api.destroy || !api.process || !api.destroy_result ||
      !api.get_num_speakers || !api.get_num_segments || !api.sort_by_start_time || !api.destroy_segment) {
    api = SherpaDiarizationApi{};
    api.loaded = true;
  }

  return api;
}

std::vector<float> read_pcm_s16le_mono_wav_samples(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("unable to open WAV: " + path.string());

  char riff[4];
  input.read(riff, 4);
  if (std::memcmp(riff, "RIFF", 4) != 0) throw std::runtime_error("WAV is not RIFF");

  std::uint32_t file_size = 0;
  input.read(reinterpret_cast<char*>(&file_size), 4);

  char wave[4];
  input.read(wave, 4);
  if (std::memcmp(wave, "WAVE", 4) != 0) throw std::runtime_error("WAV is not WAVE");

  bool fmt_seen = false, data_seen = false;
  std::uint16_t audio_format = 0, channel_count = 0, bits_per_sample = 0;
  std::uint32_t sample_rate = 0;
  std::vector<float> samples;

  while (input && !(fmt_seen && data_seen)) {
    char chunk_id[4];
    input.read(chunk_id, 4);
    if (!input) break;

    std::uint32_t chunk_size = 0;
    input.read(reinterpret_cast<char*>(&chunk_size), 4);
    if (!input) break;

    if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
      input.read(reinterpret_cast<char*>(&audio_format), 2);
      input.read(reinterpret_cast<char*>(&channel_count), 2);
      input.read(reinterpret_cast<char*>(&sample_rate), 4);
      input.seekg(6, std::ios::cur);
      input.read(reinterpret_cast<char*>(&bits_per_sample), 2);
      if (chunk_size > 16) input.seekg(chunk_size - 16, std::ios::cur);
      fmt_seen = true;
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      const std::size_t sample_count = chunk_size / 2;
      samples.reserve(sample_count);
      for (std::size_t i = 0; i < sample_count; ++i) {
        std::int16_t raw = 0;
        input.read(reinterpret_cast<char*>(&raw), 2);
        samples.push_back(static_cast<float>(raw) / 32768.0f);
      }
      data_seen = true;
    } else {
      input.seekg(chunk_size + (chunk_size % 2), std::ios::cur);
    }
  }

  if (!fmt_seen || !data_seen) throw std::runtime_error("WAV missing fmt or data");
  if (audio_format != 1) throw std::runtime_error("WAV must be PCM");
  if (channel_count != 1) throw std::runtime_error("WAV must be mono");
  if (sample_rate != 16000) throw std::runtime_error("WAV must be 16kHz");
  if (bits_per_sample != 16) throw std::runtime_error("WAV must be 16-bit");

  return samples;
}

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
  float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  if (norm_a < 1e-12f || norm_b < 1e-12f) return 0.0f;
  return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

void normalize_embedding(std::vector<float>& v) {
  float norm = 0.0f;
  for (float x : v) norm += x * x;
  norm = std::sqrt(norm);
  if (norm > 1e-12f) {
    for (float& x : v) x /= norm;
  }
}

std::vector<float> compute_cluster_embedding(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& all_samples,
    const std::vector<SherpaDiarizationSegment>& cluster_segments) {

  std::vector<std::vector<float>> segment_embeddings;
  std::vector<float> segment_durations;

  for (const auto& seg : cluster_segments) {
    int32_t start_sample = static_cast<int32_t>(seg.start_sec * 16000.0f);
    int32_t end_sample = static_cast<int32_t>(seg.end_sec * 16000.0f);
    if (start_sample < 0) start_sample = 0;
    if (end_sample > static_cast<int32_t>(all_samples.size()))
      end_sample = static_cast<int32_t>(all_samples.size());
    int32_t num_samples = end_sample - start_sample;
    if (num_samples < 1600) continue;

    const SherpaOnnxOnlineStream* stream = api.emb_create_stream(extractor);
    if (!stream) continue;

    api.stream_accept(stream, 16000, all_samples.data() + start_sample, num_samples);
    api.stream_input_finished(stream);

    std::vector<float> embedding;
    if (api.emb_is_ready(extractor, stream)) {
      const float* emb = api.emb_compute(extractor, stream);
      if (emb) {
        embedding.assign(emb, emb + embedding_dim);
        api.emb_destroy_vec(emb);
      }
    }
    api.stream_destroy(stream);

    if (!embedding.empty()) {
      normalize_embedding(embedding);
      segment_embeddings.push_back(std::move(embedding));
      segment_durations.push_back(static_cast<float>(num_samples));
    }
  }

  if (segment_embeddings.empty()) {
    return std::vector<float>(embedding_dim, 0.0f);
  }

  std::vector<float> centroid(embedding_dim, 0.0f);
  float total_duration = 0.0f;
  for (std::size_t i = 0; i < segment_embeddings.size(); ++i) {
    float dur = segment_durations[i];
    for (int32_t d = 0; d < embedding_dim; ++d) {
      centroid[d] += segment_embeddings[i][d] * dur;
    }
    total_duration += dur;
  }
  if (total_duration > 0.0f) {
    for (int32_t d = 0; d < embedding_dim; ++d) centroid[d] /= total_duration;
  }
  normalize_embedding(centroid);
  return centroid;
}

}  // namespace

bool is_sherpa_diarization_available() {
  const SherpaDiarizationApi& api = get_api();
  return api.lib_handle != nullptr && api.create != nullptr;
}

SherpaDiarizationResult run_sherpa_diarization(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir) {
  SherpaDiarizationResult result;

  const SherpaDiarizationApi& api = get_api();
  if (!api.lib_handle || !api.create) {
    result.blockers.push_back("sherpa-onnx C API library not loaded");
    return result;
  }

  const std::filesystem::path segmentation_model =
      model_dir / "sherpa-onnx-pyannote-segmentation-3-0" / "model.onnx";
  const std::filesystem::path embedding_model =
      model_dir / "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";

  if (!std::filesystem::exists(segmentation_model)) {
    result.blockers.push_back("segmentation model not found: " + segmentation_model.string());
    return result;
  }
  if (!std::filesystem::exists(embedding_model)) {
    result.blockers.push_back("embedding model not found: " + embedding_model.string());
    return result;
  }

  std::vector<float> samples;
  try {
    samples = read_pcm_s16le_mono_wav_samples(wav_path);
  } catch (const std::exception& e) {
    result.blockers.push_back(std::string("failed to read WAV: ") + e.what());
    return result;
  }

  if (samples.empty()) {
    result.blockers.push_back("WAV has no audio samples");
    return result;
  }

  const std::string seg_path = segmentation_model.string();
  const std::string emb_path = embedding_model.string();

  SherpaOnnxOfflineSpeakerDiarizationConfig config;
  std::memset(&config, 0, sizeof(config));
  config.segmentation.pyannote.model = seg_path.c_str();
  config.segmentation.num_threads = 1;
  config.segmentation.debug = 0;
  config.segmentation.provider = "cpu";
  config.embedding.model = emb_path.c_str();
  config.embedding.num_threads = 1;
  config.embedding.debug = 0;
  config.embedding.provider = "cpu";
  config.clustering.num_clusters = -1;
  config.clustering.threshold = 0.5f;  // permissive — over-segment intentionally
  config.min_duration_on = 0.3f;
  config.min_duration_off = 0.5f;

  const void* sd = api.create(&config);
  if (!sd) {
    result.blockers.push_back("sherpa-onnx failed to create diarization pipeline (config validation failed)");
    return result;
  }

  const void* diar_result = api.process(sd, samples.data(), static_cast<int32_t>(samples.size()));
  if (!diar_result) {
    result.blockers.push_back("sherpa-onnx diarization process returned null");
    api.destroy(sd);
    return result;
  }

  int32_t preliminary_speakers = api.get_num_speakers(diar_result);
  const int32_t num_segments = api.get_num_segments(diar_result);

  const SherpaOnnxOfflineSpeakerDiarizationSegment* seg_array =
      reinterpret_cast<const SherpaOnnxOfflineSpeakerDiarizationSegment*>(
          api.sort_by_start_time(diar_result));

  std::vector<SherpaDiarizationSegment> preliminary_segments;
  for (int32_t i = 0; i < num_segments; ++i) {
    SherpaDiarizationSegment seg;
    seg.start_sec = seg_array[i].start;
    seg.end_sec = seg_array[i].end;
    seg.speaker_id = seg_array[i].speaker;
    preliminary_segments.push_back(seg);
  }

  api.destroy_segment(seg_array);
  api.destroy_result(diar_result);
  api.destroy(sd);

  result.preliminary_cluster_count = preliminary_speakers;

  if (preliminary_segments.empty()) {
    result.blockers.push_back("diarization produced no segments");
    return result;
  }

  // ---- Stage 2: Embedding-based cluster reconciliation ----
  bool embedding_api_available = (api.emb_create && api.emb_destroy && api.emb_dim &&
                                   api.emb_create_stream && api.stream_accept &&
                                   api.stream_input_finished && api.emb_is_ready &&
                                   api.emb_compute && api.emb_destroy_vec && api.stream_destroy);

  if (!embedding_api_available) {
    result.segments = preliminary_segments;
    result.final_speaker_count = preliminary_speakers;
    result.reconciliation_method = "embedding_api_unavailable; preliminary clusters used without reconciliation";
    result.blockers.push_back("sherpa-onnx embedding extractor C API not available");
    result.ran = true;
    return result;
  }

  SherpaOnnxSpeakerEmbeddingExtractorConfig emb_config;
  std::memset(&emb_config, 0, sizeof(emb_config));
  emb_config.model = emb_path.c_str();
  emb_config.num_threads = 1;
  emb_config.debug = 0;
  emb_config.provider = "cpu";

  const void* extractor = api.emb_create(&emb_config);
  if (!extractor) {
    result.segments = preliminary_segments;
    result.final_speaker_count = preliminary_speakers;
    result.reconciliation_method = "embedding_extractor_creation_failed; preliminary clusters used";
    result.blockers.push_back("failed to create speaker embedding extractor");
    result.ran = true;
    return result;
  }

  const int32_t embedding_dim = api.emb_dim(extractor);

  std::map<int32_t, std::vector<SherpaDiarizationSegment>> cluster_segments;
  for (const auto& seg : preliminary_segments) {
    cluster_segments[seg.speaker_id].push_back(seg);
  }

  std::vector<std::vector<float>> centroids;
  std::vector<int32_t> cluster_ids;
  for (const auto& [cluster_id, segs] : cluster_segments) {
    std::vector<float> centroid = compute_cluster_embedding(api, extractor, embedding_dim, samples, segs);
    centroids.push_back(centroid);
    cluster_ids.push_back(cluster_id);
  }

  api.emb_destroy(extractor);

  int32_t num_clusters = static_cast<int32_t>(centroids.size());

  // Build pairwise similarity matrix
  result.pairwise_similarity_matrix.assign(num_clusters, std::vector<float>(num_clusters, 0.0f));
  for (int32_t i = 0; i < num_clusters; ++i) {
    result.pairwise_similarity_matrix[i][i] = 1.0f;
    for (int32_t j = i + 1; j < num_clusters; ++j) {
      float sim = cosine_similarity(centroids[i], centroids[j]);
      result.pairwise_similarity_matrix[i][j] = sim;
      result.pairwise_similarity_matrix[j][i] = sim;
    }
  }

  // Adaptive merging: sort pairwise similarities, find largest gap
  struct PairSim { int32_t a; int32_t b; float sim; };
  std::vector<PairSim> pairs;
  for (int32_t i = 0; i < num_clusters; ++i) {
    for (int32_t j = i + 1; j < num_clusters; ++j) {
      pairs.push_back({i, j, result.pairwise_similarity_matrix[i][j]});
    }
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const PairSim& p1, const PairSim& p2) { return p1.sim > p2.sim; });

  float merge_threshold = 0.0f;
  float largest_gap = 0.0f;
  int32_t gap_index = -1;

  // Minimum gap required to consider clusters as different speakers.
  // Below this, the embedding evidence is ambiguous — all clusters merge.
  const float kMinGap = 0.25f;

  if (pairs.size() == 1) {
    // Single pair: merge if similarity is high (same speaker)
    merge_threshold = pairs[0].sim;
    gap_index = 0;
    largest_gap = 1.0f - pairs[0].sim;  // gap from 1.0 (self-similarity)
  } else {
    for (std::size_t i = 0; i + 1 < pairs.size(); ++i) {
      float gap = pairs[i].sim - pairs[i + 1].sim;
      if (gap > largest_gap) {
        largest_gap = gap;
        gap_index = static_cast<int32_t>(i);
        merge_threshold = pairs[i].sim;
      }
    }
  }

  // If the largest gap is too small, there's no clear voice separation.
  // Merge all clusters into one speaker.
  if (largest_gap < kMinGap) {
    merge_threshold = -2.0f;  // merge everything (all sims >= -2.0)
    gap_index = -2;  // signal that min-gap override was used
  }

  // Union-find for transitive merging
  std::vector<int32_t> parent(num_clusters);
  std::iota(parent.begin(), parent.end(), 0);
  auto find = [&](int32_t x) -> int32_t {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  };
  auto unite = [&](int32_t x, int32_t y) {
    int32_t px = find(x), py = find(y);
    if (px != py) parent[px] = py;
  };

  for (const auto& p : pairs) {
    bool will_merge;
    if (gap_index == -2) {
      // Min-gap override: merge everything
      will_merge = true;
    } else if (gap_index >= 0) {
      will_merge = (p.sim >= merge_threshold);
    } else {
      will_merge = false;
    }
    if (will_merge) unite(p.a, p.b);
    result.merge_decisions.push_back({cluster_ids[p.a], cluster_ids[p.b], p.sim, will_merge});
  }

  // Assign final speaker IDs
  std::map<int32_t, int32_t> root_to_final;
  int32_t next_final_id = 0;
  std::map<int32_t, int32_t> preliminary_to_final;
  for (std::size_t i = 0; i < cluster_ids.size(); ++i) {
    int32_t root = find(static_cast<int32_t>(i));
    if (root_to_final.find(root) == root_to_final.end()) {
      root_to_final[root] = next_final_id++;
    }
    preliminary_to_final[cluster_ids[i]] = root_to_final[root];
  }

  // Remap segments
  for (const auto& seg : preliminary_segments) {
    SherpaDiarizationSegment final_seg = seg;
    auto it = preliminary_to_final.find(seg.speaker_id);
    if (it != preliminary_to_final.end()) {
      final_seg.speaker_id = it->second;
    }
    result.segments.push_back(final_seg);
  }

  result.final_speaker_count = next_final_id;

  // Build method description
  std::string desc = "largest_gap_separation: sorted_similarities=[";
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (i > 0) desc += ",";
    desc += std::to_string(pairs[i].sim);
  }
  desc += "], gap_index=" + std::to_string(gap_index);
  desc += ", merge_threshold=" + std::to_string(merge_threshold);
  desc += ", largest_gap=" + std::to_string(largest_gap);
  desc += ", preliminary_clusters=" + std::to_string(preliminary_speakers);
  desc += ", final_speakers=" + std::to_string(next_final_id);
  result.reconciliation_method = std::move(desc);

  result.ran = true;
  return result;
}

}  // namespace svp::audio
