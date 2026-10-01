#pragma once

// Silero VAD speech detection for one whisper.cpp ASR chunk: the speech-only
// sample buffer whisper decodes, and the map from that buffer's timeline
// back to chunk time. Internal to svp-audio; built only with whisper.cpp.

#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)

#include <whisper.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace svp::audio {

struct VadTimeMappingPoint {
  std::int64_t processed_centiseconds = 0;
  std::int64_t original_centiseconds = 0;
};

struct VadTimeMapper {
  std::vector<VadTimeMappingPoint> points;
  std::vector<std::pair<std::int64_t, std::int64_t>> processed_speech_ranges;

  [[nodiscard]] bool same_speech_region(std::int64_t first_centiseconds,
                                        std::int64_t second_centiseconds) const;

  [[nodiscard]] std::optional<std::int64_t> speech_region_start(
      std::int64_t processed_centiseconds) const;

  // Maps a processed-buffer timestamp back to original time. Inside a
  // synthetic gap the mapping interpolates proportionally into the removed
  // silence, which scatters word times across pauses that no longer exist
  // in the output; instead onsets resolve to the following speech region's
  // start and offsets to the preceding region's end.
  [[nodiscard]] std::int64_t map_centiseconds(
      std::int64_t processed_centiseconds, bool is_onset) const;
};

struct VadSpeechInput {
  VadTimeMapper time_mapper;
  // The speech-only buffer whisper.cpp 1.8.6 whisper_full() would decode
  // with its built-in VAD enabled. Empty when VAD found no speech.
  std::vector<float> speech_samples;
};

// Runs Silero VAD once on a chunk-local VAD context and derives both the
// speech buffer whisper decodes and the map from its timeline back to chunk
// time, so the two can never disagree and no VAD state outlives the chunk.
[[nodiscard]] VadSpeechInput detect_vad_speech(
    const std::filesystem::path& vad_model_path,
    const std::vector<float>& samples,
    const whisper_vad_params& vad_params,
    int vad_threads);

}  // namespace svp::audio

#endif
