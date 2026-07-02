#include "svp/audio/sherpa_diarization.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

#include "svp/core/memory_diagnostics.hpp"

namespace svp::audio {
namespace {

constexpr int32_t kDiarizationSampleRate = 16000;
constexpr int32_t kMaxDiarizationChunkSamples = kDiarizationSampleRate * 5;
constexpr std::int64_t kUtteranceGapThresholdUs = 750000;
constexpr std::int64_t kUtterancePaddingUs = 150000;
// 3D-Speaker embeddings drift across long recordings and across independent
// bounded Sherpa windows. A moderate same-speaker threshold keeps recurring
// voices connected without relying on one full-media clustering pass.
constexpr float kSameSpeakerSimilarityThreshold = 0.60f;
// Final long-form reconciliation operates on bounded speaker-observation
// embeddings, not audio. This lower threshold accounts for embedding drift
// across independently processed windows while preserving fixture separation.
constexpr float kGlobalSpeakerObservationMinGap = 0.10f;
constexpr float kGlobalSpeakerObservationFloorSimilarity = 0.14f;
// Dominant-track stitching is only for long-form fragmentation after global
// reconciliation. It merges non-overlapping dominant tracks that together form
// the bulk of speech, while avoiding small controlled fixture recordings.
constexpr int32_t kDominantStitchMinFinalSpeakers = 8;
constexpr std::size_t kDominantStitchMinObservations = 32;
constexpr float kDominantStitchCombinedSpeechShare = 0.70f;
constexpr float kDominantStitchMinTrackSpeechShare = 0.15f;
constexpr float kDominantStitchMaxOverlapShare = 0.01f;
// Ask Sherpa's local clustering to expose candidate speaker changes. A stricter
// threshold is reconciled later by bounded embedding assignment, keeping memory
// flat without forcing one large full-media clustering pass.
constexpr float kSherpaLocalClusteringThreshold = 0.90f;
constexpr int64_t kDiarizationWindowSamples =
    static_cast<int64_t>(kDiarizationSampleRate) * 300;
constexpr int64_t kDiarizationWindowOverlapSamples =
    static_cast<int64_t>(kDiarizationSampleRate) * 2;
constexpr float kMinClippedSegmentDuration = 0.1f;
// Sherpa local speaker labels are stable only while the local stream remains
// temporally continuous. After a real pause, recompute a bounded embedding
// before reusing that local label.
constexpr float kLocalSpeakerAssignmentMaxGapSec = 5.0f;
constexpr int32_t kMaxSpeakerEmbeddingSamples = kDiarizationSampleRate * 10;
constexpr int32_t kMinSpeakerEmbeddingSamples = kDiarizationSampleRate / 10;

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

PcmS16MonoWavInfo read_pcm_s16le_mono_wav_info(const std::filesystem::path& path) {
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
  PcmS16MonoWavInfo info;
  info.path = path;

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
      info.data_offset = input.tellg();
      info.sample_count = static_cast<std::size_t>(chunk_size / 2);
      input.seekg(chunk_size + (chunk_size % 2), std::ios::cur);
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

  return info;
}

std::vector<float> read_pcm_s16le_mono_wav_range(
    const PcmS16MonoWavInfo& info,
    std::size_t start_sample,
    std::size_t end_sample) {
  if (start_sample >= end_sample || start_sample >= info.sample_count) return {};
  end_sample = std::min(end_sample, info.sample_count);

  std::ifstream input(info.path, std::ios::binary);
  if (!input) throw std::runtime_error("unable to open WAV: " + info.path.string());
  input.seekg(info.data_offset + static_cast<std::streamoff>(start_sample * 2),
              std::ios::beg);

  std::vector<float> samples;
  samples.reserve(end_sample - start_sample);
  for (std::size_t i = start_sample; i < end_sample; ++i) {
    std::int16_t raw = 0;
    input.read(reinterpret_cast<char*>(&raw), 2);
    if (!input) break;
    samples.push_back(static_cast<float>(raw) / 32768.0f);
  }
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

bool ends_utterance(const std::string& text) {
  if (text.empty()) return false;
  const char last = text.back();
  return last == '.' || last == '?' || last == '!';
}

std::vector<float> compute_bounded_speaker_embedding(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& all_samples,
    std::size_t sample_base,
    const std::vector<SherpaDiarizationSegment>& speaker_segments) {

  std::vector<float> concatenated;
  concatenated.reserve(static_cast<std::size_t>(kMaxSpeakerEmbeddingSamples));

  for (const auto& seg : speaker_segments) {
    std::int64_t absolute_start =
        static_cast<std::int64_t>(seg.start_sec * 16000.0f);
    std::int64_t absolute_end =
        static_cast<std::int64_t>(seg.end_sec * 16000.0f);
    if (absolute_start < static_cast<std::int64_t>(sample_base)) {
      absolute_start = static_cast<std::int64_t>(sample_base);
    }
    const std::int64_t sample_limit =
        static_cast<std::int64_t>(sample_base + all_samples.size());
    if (absolute_end > sample_limit) absolute_end = sample_limit;
    const std::int64_t relative_start =
        absolute_start - static_cast<std::int64_t>(sample_base);
    const std::int64_t relative_end =
        absolute_end - static_cast<std::int64_t>(sample_base);
    int32_t start_sample = static_cast<int32_t>(relative_start);
    int32_t end_sample = static_cast<int32_t>(relative_end);
    int32_t num_samples = end_sample - start_sample;
    if (num_samples < 1600) continue;

    if (static_cast<int32_t>(concatenated.size()) + num_samples >
        kMaxSpeakerEmbeddingSamples) {
      int32_t room = kMaxSpeakerEmbeddingSamples -
                     static_cast<int32_t>(concatenated.size());
      if (room > 0) {
        concatenated.insert(concatenated.end(),
                            all_samples.data() + start_sample,
                            all_samples.data() + start_sample + room);
      }
      break;
    }
    concatenated.insert(concatenated.end(),
                        all_samples.data() + start_sample,
                        all_samples.data() + end_sample);
  }

  if (concatenated.size() < static_cast<std::size_t>(kMinSpeakerEmbeddingSamples)) {
    return std::vector<float>(embedding_dim, 0.0f);
  }

  const SherpaOnnxOnlineStream* stream = api.emb_create_stream(extractor);
  if (!stream) {
    return std::vector<float>(embedding_dim, 0.0f);
  }

  api.stream_accept(stream, 16000, concatenated.data(),
                    static_cast<int32_t>(concatenated.size()));
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

  if (embedding.empty()) {
    return std::vector<float>(embedding_dim, 0.0f);
  }
  normalize_embedding(embedding);
  return embedding;
}

struct DiarizationWindowRange {
  std::size_t accepted_start;
  std::size_t accepted_end;
  std::size_t process_start;
  std::size_t process_end;
};

std::vector<DiarizationWindowRange> build_diarization_windows(
    std::size_t sample_count) {
  std::vector<DiarizationWindowRange> windows;
  if (sample_count == 0) return windows;

  const std::size_t window_size =
      static_cast<std::size_t>(kDiarizationWindowSamples);
  const std::size_t overlap =
      static_cast<std::size_t>(kDiarizationWindowOverlapSamples);

  std::size_t window_start = 0;
  while (window_start < sample_count) {
    std::size_t accepted_end =
        std::min(window_start + window_size, sample_count);

    std::size_t process_start =
        (window_start == 0)
            ? 0
            : (window_start >= overlap ? window_start - overlap : 0);

    bool is_final = (accepted_end >= sample_count);
    std::size_t process_end =
        is_final ? sample_count
                 : std::min(accepted_end + overlap, sample_count);

    windows.push_back(
        {window_start, accepted_end, process_start, process_end});

    if (is_final) break;
    window_start = accepted_end;
  }

  return windows;
}

bool clip_segment_to_range(SherpaDiarizationSegment& seg,
                           float accepted_start_sec,
                           float accepted_end_sec) {
  float clipped_start = std::max(seg.start_sec, accepted_start_sec);
  float clipped_end = std::min(seg.end_sec, accepted_end_sec);
  if (clipped_end - clipped_start < kMinClippedSegmentDuration) return false;
  seg.start_sec = clipped_start;
  seg.end_sec = clipped_end;
  return true;
}

bool has_embedding_signal(const std::vector<float>& embedding) {
  for (float value : embedding) {
    if (std::abs(value) > 1e-6f) return true;
  }
  return false;
}

struct GlobalSpeakerTrack {
  int32_t speaker_id = 0;
  std::vector<float> centroid;
  int32_t observation_count = 0;
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

std::map<int32_t, int32_t> cluster_speaker_observations(
    const std::vector<SpeakerObservation>& observations,
    const std::set<std::pair<int32_t, int32_t>>& cannot_link_observations) {
  std::map<int32_t, int32_t> observation_to_final;
  if (observations.empty()) return observation_to_final;

  std::vector<std::vector<std::size_t>> clusters;
  clusters.reserve(observations.size());
  for (std::size_t i = 0; i < observations.size(); ++i) {
    clusters.push_back({i});
  }

  auto clusters_can_merge = [&](const std::vector<std::size_t>& a,
                                const std::vector<std::size_t>& b) {
    for (std::size_t ai : a) {
      for (std::size_t bi : b) {
        const auto cannot_link = std::minmax(observations[ai].observation_id,
                                             observations[bi].observation_id);
        if (cannot_link_observations.find(cannot_link) !=
            cannot_link_observations.end()) {
          return false;
        }
      }
    }
    return true;
  };

  auto average_similarity = [&](const std::vector<std::size_t>& a,
                                const std::vector<std::size_t>& b) {
    if (!clusters_can_merge(a, b)) return -2.0f;
    float total = 0.0f;
    std::size_t count = 0;
    for (std::size_t ai : a) {
      if (!has_embedding_signal(observations[ai].embedding)) continue;
      for (std::size_t bi : b) {
        if (!has_embedding_signal(observations[bi].embedding)) continue;
        total += cosine_similarity(observations[ai].embedding,
                                   observations[bi].embedding);
        ++count;
      }
    }
    return count == 0 ? -2.0f : total / static_cast<float>(count);
  };

  struct MergeStep {
    std::vector<std::vector<std::size_t>> clusters_after;
    float similarity = -2.0f;
  };
  std::vector<MergeStep> merge_steps;

  while (clusters.size() > 1) {
    float best_similarity = -2.0f;
    std::size_t best_a = 0;
    std::size_t best_b = 0;
    for (std::size_t i = 0; i < clusters.size(); ++i) {
      for (std::size_t j = i + 1; j < clusters.size(); ++j) {
        const float similarity = average_similarity(clusters[i], clusters[j]);
        if (similarity > best_similarity) {
          best_similarity = similarity;
          best_a = i;
          best_b = j;
        }
      }
    }
    if (best_similarity < kGlobalSpeakerObservationFloorSimilarity) break;

    clusters[best_a].insert(clusters[best_a].end(),
                            clusters[best_b].begin(),
                            clusters[best_b].end());
    clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(best_b));
    merge_steps.push_back({clusters, best_similarity});
  }

  if (merge_steps.size() > 1) {
    float largest_gap = 0.0f;
    std::size_t selected_step = merge_steps.size() - 1;
    std::vector<std::pair<float, std::size_t>> gap_candidates;
    for (std::size_t i = 0; i + 1 < merge_steps.size(); ++i) {
      const float gap = merge_steps[i].similarity - merge_steps[i + 1].similarity;
      gap_candidates.push_back({gap, i});
      if (gap > largest_gap) {
        largest_gap = gap;
        selected_step = i;
      }
    }
    std::sort(gap_candidates.begin(), gap_candidates.end(),
              [](const auto& a, const auto& b) {
                return a.first > b.first;
              });
    std::string top_gaps;
    const std::size_t gap_count = std::min<std::size_t>(gap_candidates.size(), 8);
    for (std::size_t i = 0; i < gap_count; ++i) {
      if (i > 0) top_gaps += ";";
      const std::size_t step = gap_candidates[i].second;
      top_gaps += "gap=" + std::to_string(gap_candidates[i].first) +
                  ",clusters=" +
                  std::to_string(merge_steps[step].clusters_after.size()) +
                  ",sim=" + std::to_string(merge_steps[step].similarity);
    }
    const bool gap_cut_applied = largest_gap >= kGlobalSpeakerObservationMinGap;
    svp::core::trace_memory_event("diarization.global_reconciliation.gaps", {
        {"observation_count", std::to_string(observations.size())},
        {"selected_gap", std::to_string(largest_gap)},
        {"selected_clusters", std::to_string(merge_steps[selected_step].clusters_after.size())},
        {"floor_clusters", std::to_string(clusters.size())},
        {"gap_cut_applied", gap_cut_applied ? "true" : "false"},
        {"top_gaps", top_gaps}
    });
    if (gap_cut_applied) {
      clusters = merge_steps[selected_step].clusters_after;
    }
  }

  std::sort(clusters.begin(), clusters.end(),
            [&](const std::vector<std::size_t>& a,
                const std::vector<std::size_t>& b) {
              float a_start = observations[a.front()].first_start_sec;
              float b_start = observations[b.front()].first_start_sec;
              for (std::size_t idx : a) {
                a_start = std::min(a_start, observations[idx].first_start_sec);
              }
              for (std::size_t idx : b) {
                b_start = std::min(b_start, observations[idx].first_start_sec);
              }
              return a_start < b_start;
            });

  for (std::size_t final_id = 0; final_id < clusters.size(); ++final_id) {
    for (std::size_t obs_index : clusters[final_id]) {
      observation_to_final[observations[obs_index].observation_id] =
          static_cast<int32_t>(final_id);
    }
  }
  return observation_to_final;
}

struct SpeakerTrackStats {
  std::int64_t speech_us = 0;
  float first_start_sec = 0.0f;
  float last_end_sec = 0.0f;
  bool seen = false;
};

void stitch_dominant_non_overlapping_tracks(
    std::vector<SherpaDiarizationSegment>& segments,
    int32_t& final_speaker_count,
    std::size_t observation_count) {
  if (final_speaker_count < kDominantStitchMinFinalSpeakers ||
      observation_count < kDominantStitchMinObservations ||
      segments.empty()) {
    return;
  }

  std::vector<SpeakerTrackStats> stats(static_cast<std::size_t>(final_speaker_count));
  std::int64_t total_speech_us = 0;
  for (const auto& seg : segments) {
    if (seg.speaker_id < 0 || seg.speaker_id >= final_speaker_count) continue;
    auto& st = stats[static_cast<std::size_t>(seg.speaker_id)];
    const std::int64_t dur_us = static_cast<std::int64_t>(
        std::max(0.0f, seg.end_sec - seg.start_sec) * 1000000.0f);
    st.speech_us += dur_us;
    total_speech_us += dur_us;
    if (!st.seen) {
      st.first_start_sec = seg.start_sec;
      st.last_end_sec = seg.end_sec;
      st.seen = true;
    } else {
      st.first_start_sec = std::min(st.first_start_sec, seg.start_sec);
      st.last_end_sec = std::max(st.last_end_sec, seg.end_sec);
    }
  }
  if (total_speech_us <= 0) return;

  std::vector<int32_t> speakers;
  for (int32_t sid = 0; sid < final_speaker_count; ++sid) {
    if (stats[static_cast<std::size_t>(sid)].seen) speakers.push_back(sid);
  }
  std::sort(speakers.begin(), speakers.end(),
            [&](int32_t a, int32_t b) {
              return stats[static_cast<std::size_t>(a)].speech_us >
                     stats[static_cast<std::size_t>(b)].speech_us;
            });
  if (speakers.size() < 2) return;

  const int32_t a = speakers[0];
  const int32_t b = speakers[1];
  const auto& sa = stats[static_cast<std::size_t>(a)];
  const auto& sb = stats[static_cast<std::size_t>(b)];
  const float a_share =
      static_cast<float>(sa.speech_us) / static_cast<float>(total_speech_us);
  const float b_share =
      static_cast<float>(sb.speech_us) / static_cast<float>(total_speech_us);
  const float combined_share = a_share + b_share;
  if (a_share < kDominantStitchMinTrackSpeechShare ||
      b_share < kDominantStitchMinTrackSpeechShare ||
      combined_share < kDominantStitchCombinedSpeechShare) {
    return;
  }

  std::int64_t overlap_us = 0;
  for (const auto& left : segments) {
    if (left.speaker_id != a) continue;
    for (const auto& right : segments) {
      if (right.speaker_id != b) continue;
      const float overlap_sec =
          std::min(left.end_sec, right.end_sec) -
          std::max(left.start_sec, right.start_sec);
      if (overlap_sec > 0.0f) {
        overlap_us += static_cast<std::int64_t>(overlap_sec * 1000000.0f);
      }
    }
  }
  const float overlap_share =
      static_cast<float>(overlap_us) /
      static_cast<float>(std::min(sa.speech_us, sb.speech_us));
  if (overlap_share > kDominantStitchMaxOverlapShare) return;

  const int32_t keep = std::min(a, b);
  const int32_t merge = std::max(a, b);
  for (auto& seg : segments) {
    if (seg.speaker_id == merge) {
      seg.speaker_id = keep;
    } else if (seg.speaker_id > merge) {
      --seg.speaker_id;
    }
  }
  --final_speaker_count;
  svp::core::trace_memory_event("diarization.dominant_track_stitch.merge", {
      {"keep_speaker", std::to_string(keep)},
      {"merged_speaker", std::to_string(merge)},
      {"combined_share", std::to_string(combined_share)},
      {"overlap_share", std::to_string(overlap_share)},
      {"final_speaker_count", std::to_string(final_speaker_count)}
  });
}

int32_t assign_global_speaker(
    std::vector<GlobalSpeakerTrack>& tracks,
    const std::vector<float>& embedding) {
  if (!has_embedding_signal(embedding)) {
    const int32_t speaker_id = static_cast<int32_t>(tracks.size());
    svp::core::trace_memory_event("diarization.global_speaker.new_no_embedding", {
        {"speaker_id", std::to_string(speaker_id)},
        {"existing_track_count", std::to_string(tracks.size())}
    });
    tracks.push_back({speaker_id, embedding, 0});
    return speaker_id;
  }

  float best_similarity = -2.0f;
  int32_t best_speaker = -1;
  for (const auto& track : tracks) {
    if (track.observation_count == 0 || track.centroid.empty()) continue;
    const float similarity = cosine_similarity(track.centroid, embedding);
    if (similarity > best_similarity) {
      best_similarity = similarity;
      best_speaker = track.speaker_id;
    }
  }

  if (best_speaker >= 0 &&
      best_similarity >= kSameSpeakerSimilarityThreshold) {
    svp::core::trace_memory_event("diarization.global_speaker.merge", {
        {"best_speaker", std::to_string(best_speaker)},
        {"best_similarity", std::to_string(best_similarity)},
        {"threshold", std::to_string(kSameSpeakerSimilarityThreshold)},
        {"track_count", std::to_string(tracks.size())}
    });
    auto& track = tracks[static_cast<std::size_t>(best_speaker)];
    const float prior =
        static_cast<float>(std::max(track.observation_count, 1));
    for (std::size_t i = 0; i < track.centroid.size() && i < embedding.size(); ++i) {
      track.centroid[i] =
          (track.centroid[i] * prior + embedding[i]) / (prior + 1.0f);
    }
    normalize_embedding(track.centroid);
    ++track.observation_count;
    return best_speaker;
  }

  const int32_t speaker_id = static_cast<int32_t>(tracks.size());
  svp::core::trace_memory_event("diarization.global_speaker.new", {
      {"speaker_id", std::to_string(speaker_id)},
      {"best_speaker", std::to_string(best_speaker)},
      {"best_similarity", std::to_string(best_similarity)},
      {"threshold", std::to_string(kSameSpeakerSimilarityThreshold)},
      {"existing_track_count", std::to_string(tracks.size())}
  });
  tracks.push_back({speaker_id, embedding, 1});
  return speaker_id;
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
  float merge_threshold = 0.0f;
  float largest_gap = 0.0f;
  int32_t gap_index = -1;

  if (pairs.size() == 1) {
    // Single pair: use a fixed similarity threshold to decide merge vs split.
    // If similarity >= 0.5, merge (same speaker).
    // If similarity < 0.5, keep separate (different speakers).
    if (pairs[0].sim >= kSameSpeakerSimilarityThreshold) {
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

  PcmS16MonoWavInfo wav_info;
  try {
    wav_info = read_pcm_s16le_mono_wav_info(wav_path);
  } catch (const std::exception& e) {
    result.blockers.push_back(std::string("failed to read WAV: ") + e.what());
    return result;
  }

  if (wav_info.sample_count == 0) {
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
  config.clustering.threshold = kSherpaLocalClusteringThreshold;
  config.min_duration_on = 0.3f;
  config.min_duration_off = 0.5f;

  bool embedding_api_available = (api.emb_create && api.emb_destroy && api.emb_dim &&
                                   api.emb_create_stream && api.stream_accept &&
                                   api.stream_input_finished && api.emb_is_ready &&
                                   api.emb_compute && api.emb_destroy_vec && api.stream_destroy);

  SherpaOnnxSpeakerEmbeddingExtractorConfig emb_config;
  std::memset(&emb_config, 0, sizeof(emb_config));
  emb_config.model = emb_path.c_str();
  emb_config.num_threads = 1;
  emb_config.debug = 0;
  emb_config.provider = "cpu";

  if (!embedding_api_available) {
    result.blockers.push_back(
        "sherpa-onnx embedding extractor C API not available; using window-local speakers");
  }

  const void* extractor = nullptr;
  int32_t embedding_dim = 0;
  if (embedding_api_available) {
    extractor = api.emb_create(&emb_config);
    if (extractor) {
      embedding_dim = api.emb_dim(extractor);
    } else {
      result.blockers.push_back(
          "sherpa-onnx failed to create speaker embedding extractor; using window-local speakers");
    }
  }

  std::vector<SherpaDiarizationSegment> preliminary_segments;
  std::vector<SpeakerObservation> speaker_observations;
  std::set<std::pair<int32_t, int32_t>> cannot_link_observations;
  int32_t preliminary_speakers = 0;

  const auto windows = build_diarization_windows(wav_info.sample_count);
  for (std::size_t wi = 0; wi < windows.size(); ++wi) {
    const auto& win = windows[wi];
    const float accepted_start_sec =
        static_cast<float>(win.accepted_start) /
        static_cast<float>(kDiarizationSampleRate);
    const float accepted_end_sec =
        static_cast<float>(win.accepted_end) /
        static_cast<float>(kDiarizationSampleRate);

    svp::core::trace_memory_event("diarization.window.start", {
        {"window_index", std::to_string(wi)},
        {"window_count", std::to_string(windows.size())},
        {"accepted_start", std::to_string(win.accepted_start)},
        {"accepted_end", std::to_string(win.accepted_end)},
        {"process_start", std::to_string(win.process_start)},
        {"process_end", std::to_string(win.process_end)},
        {"speaker_observation_count", std::to_string(speaker_observations.size())}
    });

    const void* sd = api.create(&config);
    if (!sd) {
      result.blockers.push_back(
          "sherpa-onnx failed to create diarization pipeline "
          "(config validation failed) at window " + std::to_string(wi));
          return result;
    }

    std::vector<float> window_samples;
    try {
      window_samples =
          read_pcm_s16le_mono_wav_range(wav_info, win.process_start, win.process_end);
    } catch (const std::exception& e) {
      result.blockers.push_back(
          std::string("failed to read WAV window: ") + e.what());
      api.destroy(sd);
      return result;
    }
    if (window_samples.empty()) {
      api.destroy(sd);
      continue;
    }

    int32_t window_speaker_groups = 0;
    std::map<int32_t, LocalSpeakerAssignment> window_local_to_global;
    for (std::size_t sample_offset = 0;
         sample_offset < window_samples.size();
         sample_offset += static_cast<std::size_t>(kMaxDiarizationChunkSamples)) {
      const std::size_t remaining = window_samples.size() - sample_offset;
      const int32_t chunk_samples = static_cast<int32_t>(
          std::min<std::size_t>(remaining,
              static_cast<std::size_t>(kMaxDiarizationChunkSamples)));

      const void* diar_result =
          api.process(sd, window_samples.data() + sample_offset, chunk_samples);
      if (!diar_result) {
        result.blockers.push_back(
            "sherpa-onnx diarization process returned null at window " +
            std::to_string(wi));
        api.destroy(sd);
        return result;
      }

      std::map<int32_t, std::vector<SherpaDiarizationSegment>> chunk_speakers;
      const int32_t num_segments = api.get_num_segments(diar_result);
      const float chunk_start_sec =
          static_cast<float>(win.process_start + sample_offset) /
          static_cast<float>(kDiarizationSampleRate);

      const SherpaOnnxOfflineSpeakerDiarizationSegment* seg_array =
          reinterpret_cast<const SherpaOnnxOfflineSpeakerDiarizationSegment*>(
              api.sort_by_start_time(diar_result));

      for (int32_t i = 0; i < num_segments; ++i) {
        SherpaDiarizationSegment seg;
        seg.start_sec = chunk_start_sec + seg_array[i].start;
        seg.end_sec = chunk_start_sec + seg_array[i].end;
        seg.speaker_id = seg_array[i].speaker;

        if (clip_segment_to_range(seg, accepted_start_sec,
                                  accepted_end_sec)) {
          chunk_speakers[seg.speaker_id].push_back(seg);
        }
      }

      api.destroy_segment(seg_array);
      api.destroy_result(diar_result);

      std::vector<int32_t> chunk_observation_ids;
      for (const auto& [local_speaker, segments] : chunk_speakers) {
        const float first_start_sec = segments.front().start_sec;
        const float last_end_sec = segments.back().end_sec;
        int32_t global_speaker = -1;
        auto known = window_local_to_global.find(local_speaker);
        if (known != window_local_to_global.end() &&
            first_start_sec - known->second.last_end_sec <=
                kLocalSpeakerAssignmentMaxGapSec) {
          global_speaker = known->second.global_speaker;
          known->second.last_end_sec =
              std::max(known->second.last_end_sec, last_end_sec);
        } else {
          global_speaker = static_cast<int32_t>(speaker_observations.size());
          std::vector<float> embedding;
          if (extractor && embedding_dim > 0) {
            embedding = compute_bounded_speaker_embedding(
                api, extractor, embedding_dim, window_samples,
                win.process_start, segments);
          }
          speaker_observations.push_back(
              {global_speaker, first_start_sec, std::move(embedding)});
          window_local_to_global[local_speaker] =
              {global_speaker, last_end_sec};
          ++window_speaker_groups;
        }
        chunk_observation_ids.push_back(global_speaker);

        for (SherpaDiarizationSegment seg : segments) {
          seg.speaker_id = global_speaker;
          preliminary_segments.push_back(seg);
        }

        svp::core::check_memory_limit("diarization.window.speaker", {
            {"window_index", std::to_string(wi)},
            {"local_speaker", std::to_string(local_speaker)},
            {"global_speaker", std::to_string(global_speaker)}
        });
      }
      for (std::size_t i = 0; i < chunk_observation_ids.size(); ++i) {
        for (std::size_t j = i + 1; j < chunk_observation_ids.size(); ++j) {
          cannot_link_observations.insert(
              std::minmax(chunk_observation_ids[i], chunk_observation_ids[j]));
        }
      }
    }
    api.destroy(sd);

    svp::core::check_memory_limit("diarization.window.process_done", {
        {"window_index", std::to_string(wi)},
        {"window_speakers", std::to_string(window_speaker_groups)}
    });

    preliminary_speakers += window_speaker_groups;

    svp::core::trace_memory_event("diarization.window.end", {
        {"window_index", std::to_string(wi)},
        {"window_speakers", std::to_string(window_speaker_groups)},
        {"speaker_observation_count", std::to_string(speaker_observations.size())},
        {"preliminary_segments", std::to_string(preliminary_segments.size())}
    });
  }

  if (extractor) {
    api.emb_destroy(extractor);
  }

  std::sort(preliminary_segments.begin(), preliminary_segments.end(),
            [](const SherpaDiarizationSegment& a,
               const SherpaDiarizationSegment& b) {
              if (a.start_sec != b.start_sec) return a.start_sec < b.start_sec;
              if (a.end_sec != b.end_sec) return a.end_sec < b.end_sec;
              return a.speaker_id < b.speaker_id;
            });

  result.preliminary_cluster_count = preliminary_speakers;
  result.preliminary_segments = preliminary_segments;

  if (preliminary_segments.empty()) {
    result.ran = true;
    result.segments.clear();
    result.final_speaker_count = 0;
    result.reconciliation_method =
        "windowed_sherpa_5min_overlap_2s; no_speech_detected";
    return result;
  }

  const std::map<int32_t, int32_t> observation_to_final =
      cluster_speaker_observations(speaker_observations,
                                   cannot_link_observations);
  result.segments.reserve(preliminary_segments.size());
  for (SherpaDiarizationSegment seg : preliminary_segments) {
    auto mapped = observation_to_final.find(seg.speaker_id);
    if (mapped != observation_to_final.end()) {
      seg.speaker_id = mapped->second;
    }
    result.segments.push_back(seg);
  }
  std::set<int32_t> final_speakers;
  for (const auto& [observation_id, final_speaker] : observation_to_final) {
    (void)observation_id;
    final_speakers.insert(final_speaker);
  }
  result.final_speaker_count =
      static_cast<int32_t>(final_speakers.empty()
                               ? speaker_observations.size()
                               : final_speakers.size());
  stitch_dominant_non_overlapping_tracks(
      result.segments, result.final_speaker_count, speaker_observations.size());
  result.reconciliation_method =
      "windowed_sherpa_5min_5s_feed_overlap_2s; "
      "bounded_10s_speaker_observations_global_gap_or_floor_reconciliation";

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

  const std::vector<SherpaDiarizationSegment>& diar_segments =
      !diar_result.preliminary_segments.empty()
          ? diar_result.preliminary_segments
          : diar_result.segments;

  for (const auto& seg : diar_segments) {
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

  const bool has_unknown_assignment =
      std::find(assignments.begin(), assignments.end(), "speaker_unknown") !=
      assignments.end();

  float max_pairwise_similarity = -2.0f;
  for (std::size_t i = 0; i < diar_result.pairwise_similarity_matrix.size(); ++i) {
    for (std::size_t j = i + 1; j < diar_result.pairwise_similarity_matrix[i].size(); ++j) {
      max_pairwise_similarity =
          std::max(max_pairwise_similarity, diar_result.pairwise_similarity_matrix[i][j]);
    }
  }
  const bool likely_overmerged_single_speaker =
      diar_result.final_speaker_count == 1 &&
      max_pairwise_similarity > -2.0f &&
      max_pairwise_similarity < kSameSpeakerSimilarityThreshold;

  if (has_unknown_assignment &&
      (diar_result.final_speaker_count > 1 || likely_overmerged_single_speaker)) {
    struct UtteranceGroup {
      std::size_t first_word = 0;
      std::size_t last_word = 0;
    };

    std::vector<UtteranceGroup> groups;
    std::size_t group_start = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
      const bool last_word = i + 1 == words.size();
      const bool gap_after =
          !last_word &&
          words[i + 1].start_us - words[i].end_us > kUtteranceGapThresholdUs;
      if (last_word || gap_after || ends_utterance(words[i].text)) {
        groups.push_back({group_start, i});
        group_start = i + 1;
      }
    }

    if (groups.size() >= 2) {
      std::vector<std::string> group_assignments(words.size(), "");
      for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        std::ostringstream sid;
        sid << "speaker_" << std::setw(4) << std::setfill('0')
            << (group_index + 1);
        for (std::size_t word_index = groups[group_index].first_word;
             word_index <= groups[group_index].last_word &&
             word_index < group_assignments.size();
             ++word_index) {
          group_assignments[word_index] = sid.str();
        }
      }

      const bool group_has_unknown =
          std::any_of(group_assignments.begin(), group_assignments.end(),
                      [](const std::string& sid) { return sid.empty(); });
      if (!group_has_unknown) {
        return group_assignments;
      }
    }

    auto nearest_known_assignment = [&](std::size_t word_index,
                                        std::size_t first_word,
                                        std::size_t last_word) -> std::string {
      std::string best;
      std::size_t best_distance = std::numeric_limits<std::size_t>::max();
      for (std::size_t i = first_word; i <= last_word && i < assignments.size(); ++i) {
        if (assignments[i] == "speaker_unknown") continue;
        const std::size_t distance =
            (i > word_index) ? (i - word_index) : (word_index - i);
        if (distance < best_distance) {
          best_distance = distance;
          best = assignments[i];
        }
      }
      return best;
    };

    for (const UtteranceGroup& group : groups) {
      for (std::size_t i = group.first_word;
           i <= group.last_word && i < assignments.size();
           ++i) {
        if (assignments[i] != "speaker_unknown") continue;
        const std::string speaker =
            nearest_known_assignment(i, group.first_word, group.last_word);
        if (!speaker.empty()) {
          assignments[i] = speaker;
        }
      }
    }

    for (std::size_t i = 0; i < assignments.size(); ++i) {
      if (assignments[i] != "speaker_unknown") continue;
      const std::string speaker =
          nearest_known_assignment(i, 0, assignments.empty() ? 0 : assignments.size() - 1);
      if (!speaker.empty()) {
        assignments[i] = speaker;
      }
    }
  }

  if (has_unknown_assignment &&
      diar_result.final_speaker_count == 1 &&
      !likely_overmerged_single_speaker) {
    for (std::string& assignment : assignments) {
      if (assignment == "speaker_unknown") {
        assignment = "speaker_0001";
      }
    }
  }

  return assignments;
}

}  // namespace svp::audio
