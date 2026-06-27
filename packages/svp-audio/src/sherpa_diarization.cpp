#include "svp/audio/sherpa_diarization.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace svp::audio {
namespace {

constexpr int32_t kDiarizationSampleRate = 16000;
constexpr int32_t kMaxDiarizationChunkSamples = kDiarizationSampleRate * 5;

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

SherpaLibState& lib_state() {
  static SherpaLibState state;
  return state;
}

void add_sherpa_lib_from_dir(std::vector<std::string>& candidates,
                              const std::filesystem::path& lib_dir,
                              const std::string& filename) {
  if (!std::filesystem::exists(lib_dir)) return;
  for (const auto& entry : std::filesystem::directory_iterator(lib_dir)) {
    if (!entry.is_directory()) continue;
    const std::string name = entry.path().filename().string();
    if (name.find("python3.") == std::string::npos) continue;
    // Check both site-packages and dist-packages layouts
    for (const auto& pkg_dir : {"site-packages", "dist-packages"}) {
      const std::filesystem::path sherpa_lib =
          entry.path() / pkg_dir / "sherpa_onnx" / "lib" / filename;
      if (std::filesystem::exists(sherpa_lib)) {
        candidates.push_back(sherpa_lib.string());
      }
    }
  }
}

void add_sherpa_lib_from_env(std::vector<std::string>& candidates,
                              const char* env_var,
                              const std::string& filename) {
  const char* env_val = std::getenv(env_var);
  if (!env_val || !env_val[0]) return;
  std::filesystem::path env_path(env_val);
  // venv/Conda lib directories: <env>/lib/python3.*/site-packages/sherpa_onnx/lib/
  add_sherpa_lib_from_dir(candidates, env_path / "lib", filename);
  // Also check <env>/lib/sherpa_onnx/ (some installs place libs directly)
  const std::filesystem::path direct_lib = env_path / "lib" / "sherpa_onnx" / "lib" / filename;
  if (std::filesystem::exists(direct_lib)) {
    candidates.push_back(direct_lib.string());
  }
  // Conda sometimes places libs in <env>/lib/ directly
  const std::filesystem::path conda_lib = env_path / "lib" / filename;
  if (std::filesystem::exists(conda_lib)) {
    candidates.push_back(conda_lib.string());
  }
}

std::vector<std::string> build_candidate_paths() {
  std::vector<std::string> candidates;

  // 1. Explicit path set via set_sherpa_lib_path()
  if (!lib_state().explicit_path.empty()) {
    candidates.push_back(lib_state().explicit_path);
  }

  // 2. SHERPA_ONNX_LIB_PATH env var
  const char* env_path = std::getenv("SHERPA_ONNX_LIB_PATH");
  if (env_path && env_path[0]) {
    candidates.push_back(env_path);
  }

  // 3. macOS user site-packages: ~/Library/Python/3.{9..14}/lib/python/site-packages/
  const char* home = std::getenv("HOME");
  if (home && home[0]) {
    std::string home_str(home);
    for (int minor = 9; minor <= 14; ++minor) {
      candidates.push_back(home_str +
          "/Library/Python/3." + std::to_string(minor) +
          "/lib/python/site-packages/sherpa_onnx/lib/libsherpa-onnx-c-api.dylib");
    }
    // Linux pip --user: ~/.local/lib/python3.*/site-packages/ and dist-packages/
    add_sherpa_lib_from_dir(candidates,
        std::filesystem::path(home_str) / ".local" / "lib",
        "libsherpa-onnx-c-api.so");
  }

  // 4. Virtual environments (venv, Conda)
  add_sherpa_lib_from_env(candidates, "VIRTUAL_ENV", "libsherpa-onnx-c-api.dylib");
  add_sherpa_lib_from_env(candidates, "CONDA_PREFIX", "libsherpa-onnx-c-api.dylib");
  // On Linux venvs the shared lib is .so
  add_sherpa_lib_from_env(candidates, "VIRTUAL_ENV", "libsherpa-onnx-c-api.so");
  add_sherpa_lib_from_env(candidates, "CONDA_PREFIX", "libsherpa-onnx-c-api.so");

  // 5. Homebrew site-packages (Apple Silicon and Intel)
  add_sherpa_lib_from_dir(candidates, "/opt/homebrew/lib", "libsherpa-onnx-c-api.dylib");
  add_sherpa_lib_from_dir(candidates, "/usr/local/lib", "libsherpa-onnx-c-api.dylib");

  // 6. Direct Homebrew and system library paths (macOS)
  candidates.push_back("/opt/homebrew/lib/libsherpa-onnx-c-api.dylib");
  candidates.push_back("/usr/local/lib/libsherpa-onnx-c-api.dylib");

  // 7. Linux system paths (site-packages and dist-packages scanned above via HOME)
  //    Also check common system-level Python directories
  add_sherpa_lib_from_dir(candidates, "/usr/lib", "libsherpa-onnx-c-api.so");
  add_sherpa_lib_from_dir(candidates, "/usr/local/lib", "libsherpa-onnx-c-api.so");
  candidates.push_back("/usr/local/lib/libsherpa-onnx-c-api.so");
  candidates.push_back("/usr/lib/libsherpa-onnx-c-api.so");
  candidates.push_back("/usr/lib/x86_64-linux-gnu/libsherpa-onnx-c-api.so");
  candidates.push_back("/usr/lib/aarch64-linux-gnu/libsherpa-onnx-c-api.so");

  return candidates;
}

SherpaDiarizationApi& get_api() {
  static SherpaDiarizationApi api;
  if (api.loaded) return api;
  api.loaded = true;

  std::vector<std::string> candidates = build_candidate_paths();
  lib_state().attempted_paths = candidates;

  for (const auto& path : candidates) {
    api.lib_handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (api.lib_handle) {
      lib_state().loaded_path = path;
      break;
    }
  }

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

void set_sherpa_lib_path(const std::string& path) {
  lib_state().explicit_path = path;
}

std::string sherpa_lib_path_used() {
  return lib_state().loaded_path;
}

std::vector<std::string> sherpa_lib_paths_attempted() {
  return lib_state().attempted_paths;
}

ReconciliationResult reconcile_clusters(
    const std::vector<std::vector<float>>& similarity_matrix,
    const std::vector<int32_t>& cluster_ids) {

  ReconciliationResult result;
  int32_t num_clusters = static_cast<int32_t>(similarity_matrix.size());

  if (num_clusters <= 1) {
    result.cluster_to_final[0] = 0;
    result.final_speaker_count = 1;
    result.method_description = "single cluster; no merging needed";
    return result;
  }

  // Collect pairwise similarities (upper triangle)
  struct PairSim { int32_t a; int32_t b; float sim; };
  std::vector<PairSim> pairs;
  for (int32_t i = 0; i < num_clusters; ++i) {
    for (int32_t j = i + 1; j < num_clusters; ++j) {
      pairs.push_back({i, j, similarity_matrix[i][j]});
    }
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const PairSim& p1, const PairSim& p2) { return p1.sim > p2.sim; });

  // Minimum gap required to consider clusters as different speakers.
  // Below this, the embedding evidence is ambiguous — all clusters merge.
  const float kMinGap = 0.25f;

  // Fixed similarity threshold for the single-pair case.
  // Below this, two clusters are clearly different speakers.
  // Above this, they are likely the same speaker and should merge.
  const float kSinglePairThreshold = 0.5f;

  float merge_threshold = 0.0f;
  float largest_gap = 0.0f;
  int32_t gap_index = -1;

  if (pairs.size() == 1) {
    // Single pair: use a fixed similarity threshold to decide merge vs split.
    // If similarity >= 0.5, merge (same speaker).
    // If similarity < 0.5, keep separate (different speakers).
    if (pairs[0].sim >= kSinglePairThreshold) {
      merge_threshold = pairs[0].sim;  // will merge
      gap_index = 0;
    } else {
      merge_threshold = 2.0f;  // impossible to reach — won't merge
      gap_index = -1;
    }
    largest_gap = 1.0f - pairs[0].sim;
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
  if (largest_gap < kMinGap && pairs.size() > 1) {
    merge_threshold = -2.0f;  // merge everything
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
  for (std::size_t i = 0; i < cluster_ids.size(); ++i) {
    int32_t root = find(static_cast<int32_t>(i));
    if (root_to_final.find(root) == root_to_final.end()) {
      root_to_final[root] = next_final_id++;
    }
    result.cluster_to_final[cluster_ids[i]] = root_to_final[root];
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
  desc += ", preliminary_clusters=" + std::to_string(num_clusters);
  desc += ", final_speakers=" + std::to_string(next_final_id);
  result.method_description = std::move(desc);

  return result;
}

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
    std::string blocker = "sherpa-onnx C API library not loaded. Attempted paths:";
    std::vector<std::string> attempted = sherpa_lib_paths_attempted();
    if (attempted.empty()) {
      blocker += " (none — library discovery was not triggered)";
    } else {
      for (std::size_t i = 0; i < attempted.size(); ++i) {
        blocker += "\n  [" + std::to_string(i + 1) + "] " + attempted[i];
      }
    }
    result.blockers.push_back(blocker);
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

  std::vector<SherpaDiarizationSegment> preliminary_segments;
  int32_t preliminary_speakers = 0;
  int32_t speaker_id_base = 0;

  for (std::size_t sample_offset = 0; sample_offset < samples.size();
       sample_offset += static_cast<std::size_t>(kMaxDiarizationChunkSamples)) {
    const std::size_t remaining = samples.size() - sample_offset;
    const int32_t chunk_samples = static_cast<int32_t>(
        std::min<std::size_t>(remaining, kMaxDiarizationChunkSamples));

    const void* diar_result =
        api.process(sd, samples.data() + sample_offset, chunk_samples);
    if (!diar_result) {
      result.blockers.push_back("sherpa-onnx diarization process returned null");
      api.destroy(sd);
      return result;
    }

    const int32_t chunk_speakers = api.get_num_speakers(diar_result);
    const int32_t num_segments = api.get_num_segments(diar_result);
    const float chunk_start_sec =
        static_cast<float>(sample_offset) / static_cast<float>(kDiarizationSampleRate);

    const SherpaOnnxOfflineSpeakerDiarizationSegment* seg_array =
        reinterpret_cast<const SherpaOnnxOfflineSpeakerDiarizationSegment*>(
            api.sort_by_start_time(diar_result));

    int32_t max_chunk_speaker = -1;
    for (int32_t i = 0; i < num_segments; ++i) {
      SherpaDiarizationSegment seg;
      seg.start_sec = chunk_start_sec + seg_array[i].start;
      seg.end_sec = chunk_start_sec + seg_array[i].end;
      seg.speaker_id = speaker_id_base + seg_array[i].speaker;
      max_chunk_speaker = std::max(max_chunk_speaker, seg_array[i].speaker);
      preliminary_segments.push_back(seg);
    }

    api.destroy_segment(seg_array);
    api.destroy_result(diar_result);

    speaker_id_base += std::max(chunk_speakers, max_chunk_speaker + 1);
  }

  api.destroy(sd);

  preliminary_speakers = speaker_id_base;
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

  // Reconcile clusters using adaptive largest-gap separation
  ReconciliationResult recon = reconcile_clusters(result.pairwise_similarity_matrix, cluster_ids);
  result.merge_decisions = recon.merge_decisions;
  result.reconciliation_method = recon.method_description;

  // Remap segments
  for (const auto& seg : preliminary_segments) {
    SherpaDiarizationSegment final_seg = seg;
    auto it = recon.cluster_to_final.find(seg.speaker_id);
    if (it != recon.cluster_to_final.end()) {
      final_seg.speaker_id = it->second;
    }
    result.segments.push_back(final_seg);
  }

  result.final_speaker_count = recon.final_speaker_count;

  result.cluster_centroids = centroids;
  result.cluster_ids_for_centroids = cluster_ids;
  result.cluster_to_final = recon.cluster_to_final;

  result.ran = true;
  return result;
}

std::vector<std::string> refine_word_speakers_by_embedding(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result) {

  // Return empty on failure paths so the caller falls back to
  // segment-overlap assignment instead of overriding with speaker_unknown.
  if (words.empty() || diar_result.cluster_centroids.empty()) {
    return {};
  }

  const SherpaDiarizationApi& api = get_api();
  if (!api.lib_handle || !api.emb_create) {
    return {};
  }

  const std::filesystem::path embedding_model =
      model_dir / "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";
  if (!std::filesystem::exists(embedding_model)) {
    return {};
  }

  std::vector<float> samples;
  try {
    samples = read_pcm_s16le_mono_wav_samples(wav_path);
  } catch (...) {
    return {};
  }
  if (samples.empty()) return {};

  const std::string emb_path = embedding_model.string();
  SherpaOnnxSpeakerEmbeddingExtractorConfig emb_config;
  std::memset(&emb_config, 0, sizeof(emb_config));
  emb_config.model = emb_path.c_str();
  emb_config.num_threads = 1;
  emb_config.debug = 0;
  emb_config.provider = "cpu";

  const void* extractor = api.emb_create(&emb_config);
  if (!extractor) return {};

  const int32_t embedding_dim = api.emb_dim(extractor);

  if (diar_result.cluster_centroids.empty()) {
    api.emb_destroy(extractor);
    return {};
  }

  // Compute embeddings for each diarization segment (full segment, no splitting).
  struct SubSeg {
    float start_sec;
    float end_sec;
    std::vector<float> embedding;
  };
  std::vector<SubSeg> sub_segs;

  auto compute_embedding_for_range = [&](int32_t start_sample,
                                          int32_t num_samples) -> std::vector<float> {
    if (num_samples < 1600) return {};
    const SherpaOnnxOnlineStream* stream = api.emb_create_stream(extractor);
    if (!stream) return {};
    api.stream_accept(stream, 16000, samples.data() + start_sample, num_samples);
    api.stream_input_finished(stream);
    std::vector<float> emb;
    if (api.emb_is_ready(extractor, stream)) {
      const float* result = api.emb_compute(extractor, stream);
      if (result) {
        emb.assign(result, result + embedding_dim);
        api.emb_destroy_vec(result);
      }
    }
    api.stream_destroy(stream);
    if (!emb.empty()) normalize_embedding(emb);
    return emb;
  };

  for (const auto& seg : diar_result.segments) {
    int32_t start_sample = static_cast<int32_t>(seg.start_sec * 16000.0f);
    int32_t end_sample = static_cast<int32_t>(seg.end_sec * 16000.0f);
    if (start_sample < 0) start_sample = 0;
    if (end_sample > static_cast<int32_t>(samples.size()))
      end_sample = static_cast<int32_t>(samples.size());

    std::vector<float> emb = compute_embedding_for_range(
        start_sample, end_sample - start_sample);
    if (!emb.empty()) {
      sub_segs.push_back({seg.start_sec, seg.end_sec, std::move(emb)});
    }
  }

  api.emb_destroy(extractor);

  if (sub_segs.empty()) return {};

  // Agglomerative clustering with largest-gap separation.
  // Merge clusters bottom-up, tracking the similarity at each merge.
  // Stop when we find the largest gap in merge similarities — this is
  // the natural point where two distinct speakers separate.
  std::size_t n = sub_segs.size();
  std::vector<int32_t> cluster_id(n);
  std::iota(cluster_id.begin(), cluster_id.end(), 0);

  // Compute pairwise similarities
  std::vector<std::vector<float>> sim(n, std::vector<float>(n, 0.0f));
  for (std::size_t i = 0; i < n; ++i) {
    sim[i][i] = 1.0f;
    for (std::size_t j = i + 1; j < n; ++j) {
      float s = cosine_similarity(sub_segs[i].embedding, sub_segs[j].embedding);
      sim[i][j] = s;
      sim[j][i] = s;
    }
  }

  // Cluster centroids for agglomerative merging
  std::map<int32_t, std::vector<std::size_t>> cluster_members;
  std::map<int32_t, std::vector<float>> cluster_centroids;
  for (std::size_t i = 0; i < n; ++i) {
    cluster_members[cluster_id[i]] = {i};
    cluster_centroids[cluster_id[i]] = sub_segs[i].embedding;
  }

  auto cluster_similarity = [&](int32_t c1, int32_t c2) -> float {
    float total = 0.0f;
    std::size_t count = 0;
    for (std::size_t m1 : cluster_members[c1]) {
      for (std::size_t m2 : cluster_members[c2]) {
        total += sim[m1][m2];
        ++count;
      }
    }
    return count > 0 ? total / count : -2.0f;
  };

  // Track merge similarities to find the largest gap
  std::vector<float> merge_sims;

  while (cluster_members.size() > 1) {
    float best_sim = -2.0f;
    int32_t best_c1 = -1, best_c2 = -1;
    for (auto it1 = cluster_members.begin(); it1 != cluster_members.end(); ++it1) {
      auto it2 = it1;
      ++it2;
      for (; it2 != cluster_members.end(); ++it2) {
        float s = cluster_similarity(it1->first, it2->first);
        if (s > best_sim) {
          best_sim = s;
          best_c1 = it1->first;
          best_c2 = it2->first;
        }
      }
    }
    if (best_c1 < 0) break;

    merge_sims.push_back(best_sim);

    // Merge c2 into c1
    for (std::size_t m : cluster_members[best_c2]) {
      cluster_id[m] = best_c1;
      cluster_members[best_c1].push_back(m);
    }
    auto& centroid = cluster_centroids[best_c1];
    std::fill(centroid.begin(), centroid.end(), 0.0f);
    for (std::size_t m : cluster_members[best_c1]) {
      for (int d = 0; d < embedding_dim; ++d) {
        centroid[d] += sub_segs[m].embedding[d];
      }
    }
    for (int d = 0; d < embedding_dim; ++d) {
      centroid[d] /= static_cast<float>(cluster_members[best_c1].size());
    }
    cluster_members.erase(best_c2);
    cluster_centroids.erase(best_c2);
  }

  // Find the largest gap in merge similarities to determine the cut point.
  // merge_sims is in descending order (most similar merge first). The largest
  // gap between consecutive merge sims indicates where intra-speaker merges
  // end and inter-speaker merges begin.
  // num_merges = how many of the top merges to replay (intra-speaker only).
  // We exclude the very last gap (after the second-to-last merge) because the
  // final merge always has an artificially large gap — it merges the last
  // two remaining clusters, which are naturally the most dissimilar.
  std::size_t num_merges = 0;  // default: don't merge anything (all separate)
  if (merge_sims.size() > 1) {
    float largest_gap = 0.0f;
    std::size_t gap_idx = 0;
    // Consider gaps 0..(merge_sims.size()-2), skipping only the last gap
    std::size_t num_gaps = merge_sims.size() - 1;
    // When there are 3+ merges, skip the last gap (it's artificial).
    // With 2 merges (1 gap), still consider it.
    std::size_t gaps_to_check = (num_gaps > 1) ? num_gaps - 1 : num_gaps;
    for (std::size_t i = 0; i < gaps_to_check; ++i) {
      float gap = merge_sims[i] - merge_sims[i + 1];
      if (gap > largest_gap) {
        largest_gap = gap;
        gap_idx = i;
      }
    }
    // Merges 0..gap_idx (inclusive) are intra-speaker
    num_merges = gap_idx + 1;
    // If the largest gap is too small, treat all as one speaker
    if (largest_gap < 0.10f) {
      num_merges = merge_sims.size();  // merge everything
    }
  }

  // Re-run clustering but only perform num_merges intra-speaker merges
  std::iota(cluster_id.begin(), cluster_id.end(), 0);
  cluster_members.clear();
  cluster_centroids.clear();
  for (std::size_t i = 0; i < n; ++i) {
    cluster_members[cluster_id[i]] = {i};
    cluster_centroids[cluster_id[i]] = sub_segs[i].embedding;
  }

  for (std::size_t step = 0; step < num_merges && cluster_members.size() > 1; ++step) {
    float best_sim = -2.0f;
    int32_t best_c1 = -1, best_c2 = -1;
    for (auto it1 = cluster_members.begin(); it1 != cluster_members.end(); ++it1) {
      auto it2 = it1;
      ++it2;
      for (; it2 != cluster_members.end(); ++it2) {
        float s = cluster_similarity(it1->first, it2->first);
        if (s > best_sim) {
          best_sim = s;
          best_c1 = it1->first;
          best_c2 = it2->first;
        }
      }
    }
    if (best_c1 < 0) break;

    for (std::size_t m : cluster_members[best_c2]) {
      cluster_id[m] = best_c1;
      cluster_members[best_c1].push_back(m);
    }
    cluster_members.erase(best_c2);
    cluster_centroids.erase(best_c2);
  }

  // Map cluster IDs to sequential speaker IDs (0, 1)
  // Order by first appearance in time
  std::map<int32_t, int32_t> cluster_to_speaker;
  int32_t next_speaker = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (cluster_to_speaker.find(cluster_id[i]) == cluster_to_speaker.end()) {
      cluster_to_speaker[cluster_id[i]] = next_speaker++;
    }
  }

  // Assign words to speakers based on sub-segments using max overlap
  std::vector<std::string> assignments(words.size(), "speaker_unknown");
  for (std::size_t i = 0; i < words.size(); ++i) {
    std::int64_t w_start = words[i].start_us;
    std::int64_t w_end = words[i].end_us;
    int32_t best_speaker = -1;
    std::int64_t best_overlap = 0;
    for (std::size_t j = 0; j < sub_segs.size(); ++j) {
      std::int64_t s_start = static_cast<std::int64_t>(sub_segs[j].start_sec * 1000000.0f);
      std::int64_t s_end = static_cast<std::int64_t>(sub_segs[j].end_sec * 1000000.0f);
      std::int64_t overlap = std::min(w_end, s_end) - std::max(w_start, s_start);
      if (overlap > best_overlap) {
        best_overlap = overlap;
        best_speaker = cluster_to_speaker[cluster_id[j]];
      }
    }
    if (best_speaker < 0) {
      std::int64_t nearest_dist = std::numeric_limits<std::int64_t>::max();
      for (std::size_t j = 0; j < sub_segs.size(); ++j) {
        std::int64_t s_start = static_cast<std::int64_t>(sub_segs[j].start_sec * 1000000.0f);
        std::int64_t s_end = static_cast<std::int64_t>(sub_segs[j].end_sec * 1000000.0f);
        std::int64_t dist;
        if (w_end <= s_start) dist = s_start - w_end;
        else if (w_start >= s_end) dist = w_start - s_end;
        else dist = 0;
        if (dist < nearest_dist) {
          nearest_dist = dist;
          best_speaker = cluster_to_speaker[cluster_id[j]];
        }
      }
      if (nearest_dist > 500000) best_speaker = -1;
    }
    if (best_speaker >= 0) {
      std::ostringstream sid;
      sid << "speaker_" << std::setw(4) << std::setfill('0') << (best_speaker + 1);
      assignments[i] = sid.str();
    }
  }

  return assignments;
}

}  // namespace svp::audio
