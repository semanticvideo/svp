#include "window_map.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace svp::audio {

using namespace sherpa_diarization_internal;

namespace sherpa_diarization_internal {

namespace {

// A window-local speaker's observation and the end of its latest speech.
struct WindowLocalSpeaker {
  std::size_t window_observation = 0;
  float last_end_sec = 0.0f;
};

std::size_t pieces_in(const DiarizationWindowRange& window) {
  const std::size_t samples = window.process_end - window.process_start;
  const auto piece = static_cast<std::size_t>(kMaxDiarizationChunkSamples);
  return (samples + piece - 1) / piece;
}

}  // namespace

DiarizationModelFiles diarization_model_files(const std::filesystem::path& model_dir) {
  return {.segmentation =
              (model_dir / "sherpa-onnx-pyannote-segmentation-3-0" / "model.onnx").string(),
          .embedding =
              (model_dir / "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx").string()};
}

SherpaOnnxOfflineSpeakerDiarizationConfig diarization_pipeline_config(
    const DiarizationModelFiles& files, const svp::models::SherpaThreadCounts& threads) {
  SherpaOnnxOfflineSpeakerDiarizationConfig config;
  std::memset(&config, 0, sizeof(config));
  config.segmentation.pyannote.model = files.segmentation.c_str();
  config.segmentation.num_threads = threads.segmentation;
  config.segmentation.debug = 0;
  config.segmentation.provider = "cpu";
  config.embedding.model = files.embedding.c_str();
  config.embedding.num_threads = threads.embedding;
  config.embedding.debug = 0;
  config.embedding.provider = "cpu";
  config.clustering.num_clusters = -1;
  config.clustering.threshold = kSherpaLocalClusteringThreshold;
  config.min_duration_on = 0.3f;
  config.min_duration_off = 0.5f;
  return config;
}

SherpaOnnxSpeakerEmbeddingExtractorConfig diarization_extractor_config(
    const DiarizationModelFiles& files, const svp::models::SherpaThreadCounts& threads) {
  SherpaOnnxSpeakerEmbeddingExtractorConfig config;
  std::memset(&config, 0, sizeof(config));
  config.model = files.embedding.c_str();
  config.num_threads = threads.embedding;
  config.debug = 0;
  config.provider = "cpu";
  return config;
}

bool embedding_api_available(const SherpaDiarizationApi& api) {
  return api.emb_create && api.emb_destroy && api.emb_dim && api.emb_create_stream &&
         api.stream_accept && api.stream_input_finished && api.emb_is_ready &&
         api.emb_compute && api.emb_destroy_vec && api.stream_destroy;
}

DiarizationWindowOutcome map_diarization_window(
    const SherpaDiarizationApi& api, const SherpaOnnxOfflineSpeakerDiarizationConfig& config,
    const void* extractor, int32_t embedding_dim, const PcmS16MonoWavInfo& wav_info,
    const DiarizationWindowRange& win, std::size_t wi, const std::function<void()>& on_piece) {
  DiarizationWindowOutcome outcome;
  const float accepted_start_sec =
      static_cast<float>(win.accepted_start) / static_cast<float>(kDiarizationSampleRate);
  const float accepted_end_sec =
      static_cast<float>(win.accepted_end) / static_cast<float>(kDiarizationSampleRate);

  const void* sd = api.create(&config);
  if (!sd) {
    outcome.failure = DiarizationWindowFailure{
        "sherpa-onnx failed to create diarization pipeline "
        "(config validation failed) at window " +
        std::to_string(wi)};
    return outcome;
  }

  std::vector<float> window_samples;
  try {
    window_samples = read_pcm_s16le_mono_wav_range(wav_info, win.process_start, win.process_end);
  } catch (const std::exception& e) {
    api.destroy(sd);
    outcome.failure =
        DiarizationWindowFailure{std::string("failed to read WAV window: ") + e.what()};
    return outcome;
  }
  DiarizationWindowMap map;
  map.window_index = wi;
  if (window_samples.empty()) {
    api.destroy(sd);
    outcome.map = std::move(map);
    return outcome;
  }

  std::size_t window_observations = 0;
  std::map<int32_t, WindowLocalSpeaker> window_local;
  for (std::size_t sample_offset = 0; sample_offset < window_samples.size();
       sample_offset += static_cast<std::size_t>(kMaxDiarizationChunkSamples)) {
    const std::size_t remaining = window_samples.size() - sample_offset;
    const int32_t chunk_samples = static_cast<int32_t>(
        std::min<std::size_t>(remaining, static_cast<std::size_t>(kMaxDiarizationChunkSamples)));

    const void* diar_result = api.process(sd, window_samples.data() + sample_offset, chunk_samples);
    if (!diar_result) {
      api.destroy(sd);
      outcome.failure = DiarizationWindowFailure{
          "sherpa-onnx diarization process returned null at window " + std::to_string(wi)};
      return outcome;
    }

    std::map<int32_t, std::vector<SherpaDiarizationSegment>> chunk_speakers;
    const int32_t num_segments = api.get_num_segments(diar_result);
    const float chunk_start_sec = static_cast<float>(win.process_start + sample_offset) /
                                  static_cast<float>(kDiarizationSampleRate);

    const SherpaOnnxOfflineSpeakerDiarizationSegment* seg_array =
        reinterpret_cast<const SherpaOnnxOfflineSpeakerDiarizationSegment*>(
            api.sort_by_start_time(diar_result));

    for (int32_t i = 0; i < num_segments; ++i) {
      SherpaDiarizationSegment seg;
      seg.start_sec = chunk_start_sec + seg_array[i].start;
      seg.end_sec = chunk_start_sec + seg_array[i].end;
      seg.speaker_id = seg_array[i].speaker;

      if (clip_segment_to_range(seg, accepted_start_sec, accepted_end_sec)) {
        chunk_speakers[seg.speaker_id].push_back(seg);
      }
    }

    api.destroy_segment(seg_array);
    api.destroy_result(diar_result);

    DiarizationWindowPiece piece;
    piece.speakers.reserve(chunk_speakers.size());
    for (auto& [local_speaker, segments] : chunk_speakers) {
      const float first_start_sec = segments.front().start_sec;
      const float last_end_sec = segments.back().end_sec;
      DiarizationPieceSpeaker speaker;
      speaker.local_speaker = local_speaker;
      auto known = window_local.find(local_speaker);
      if (known != window_local.end() &&
          first_start_sec - known->second.last_end_sec <= kLocalSpeakerAssignmentMaxGapSec) {
        speaker.window_observation = known->second.window_observation;
        known->second.last_end_sec = std::max(known->second.last_end_sec, last_end_sec);
      } else {
        speaker.window_observation = window_observations++;
        speaker.starts_observation = true;
        if (extractor && embedding_dim > 0) {
          speaker.embedding = compute_bounded_speaker_embedding(
              api, extractor, embedding_dim, window_samples, win.process_start, segments);
        }
        window_local[local_speaker] = {speaker.window_observation, last_end_sec};
      }
      speaker.segments = std::move(segments);
      piece.speakers.push_back(std::move(speaker));
    }
    map.pieces.push_back(std::move(piece));
    if (on_piece) {
      on_piece();
    }
  }
  api.destroy(sd);
  outcome.map = std::move(map);
  return outcome;
}

}  // namespace sherpa_diarization_internal

std::size_t diarization_window_count(std::size_t sample_count) {
  return build_diarization_windows(sample_count).size();
}

std::size_t diarization_window_piece_count(std::size_t sample_count, std::size_t window_index) {
  const std::vector<DiarizationWindowRange> windows = build_diarization_windows(sample_count);
  if (window_index >= windows.size()) {
    throw std::out_of_range("diarization window " + std::to_string(window_index) +
                            " is outside a WAV of " + std::to_string(sample_count) +
                            " samples");
  }
  return pieces_in(windows[window_index]);
}

std::size_t diarization_window_samples(std::size_t sample_count, std::size_t window_index) {
  const std::vector<DiarizationWindowRange> windows = build_diarization_windows(sample_count);
  if (window_index >= windows.size()) {
    throw std::out_of_range("diarization window " + std::to_string(window_index) +
                            " is outside a WAV of " + std::to_string(sample_count) +
                            " samples");
  }
  return windows[window_index].process_end - windows[window_index].process_start;
}

std::size_t diarization_wav_sample_count(const std::filesystem::path& wav_path) {
  return read_pcm_s16le_mono_wav_info(wav_path).sample_count;
}

DiarizationWindowOutcome run_diarization_window_map(const std::filesystem::path& wav_path,
                                                    const std::filesystem::path& model_dir,
                                                    std::size_t window_index,
                                                    const DiarizationWindowSettings& settings,
                                                    const std::function<void()>& on_piece) {
  const auto failed = [](std::string blocker) {
    DiarizationWindowOutcome outcome;
    outcome.failure = DiarizationWindowFailure{std::move(blocker)};
    return outcome;
  };
  if (settings.threads.segmentation <= 0 || settings.threads.embedding <= 0) {
    return failed("sherpa-onnx needs positive segmentation and embedding thread counts");
  }
  if (!is_sherpa_diarization_available()) {
    return failed("sherpa-onnx C API library not loaded");
  }
  const SherpaDiarizationApi& api = get_api();
  const DiarizationModelFiles files = diarization_model_files(model_dir);
  if (!std::filesystem::exists(files.segmentation)) {
    return failed("segmentation model not found: " + files.segmentation);
  }
  if (!std::filesystem::exists(files.embedding)) {
    return failed("embedding model not found: " + files.embedding);
  }
  PcmS16MonoWavInfo wav_info;
  try {
    wav_info = read_pcm_s16le_mono_wav_info(wav_path);
  } catch (const std::exception& e) {
    return failed(std::string("failed to read WAV: ") + e.what());
  }
  const std::vector<DiarizationWindowRange> windows =
      build_diarization_windows(wav_info.sample_count);
  if (window_index >= windows.size()) {
    return failed("diarization window " + std::to_string(window_index) +
                  " is outside the WAV's " + std::to_string(windows.size()) + " windows");
  }

  const void* extractor = nullptr;
  int32_t embedding_dim = 0;
  if (settings.compute_embeddings) {
    if (!embedding_api_available(api)) {
      return failed("sherpa-onnx embedding extractor C API not available");
    }
    const SherpaOnnxSpeakerEmbeddingExtractorConfig extractor_config =
        diarization_extractor_config(files, settings.threads);
    extractor = api.emb_create(&extractor_config);
    if (!extractor) {
      return failed("sherpa-onnx failed to create speaker embedding extractor");
    }
    embedding_dim = api.emb_dim(extractor);
  }
  struct ExtractorOwner {
    const SherpaDiarizationApi& api;
    const void* extractor;
    ~ExtractorOwner() {
      if (extractor) api.emb_destroy(extractor);
    }
  } owner{api, extractor};

  const SherpaOnnxOfflineSpeakerDiarizationConfig config =
      diarization_pipeline_config(files, settings.threads);
  return map_diarization_window(api, config, extractor, embedding_dim, wav_info,
                                windows[window_index], window_index, on_piece);
}

}  // namespace svp::audio
