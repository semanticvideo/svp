#include "whisper_vad_speech.hpp"

#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace svp::audio {
namespace {

constexpr std::int64_t kWhisperCentisecondsPerSecond = 100;
// whisper.cpp 1.8.6 whisper_vad() separates packed speech segments with
// 0.1 s of silence.
constexpr std::int64_t kWhisperVadProcessedGapMilliseconds = 100;
// A VAD mapping segment whose original span exceeds its processed span by
// more than this is a synthetic gap covering removed silence, not speech.
constexpr std::int64_t kVadSyntheticGapSlackCentiseconds = 5;

struct WhisperVadContextDeleter {
  void operator()(whisper_vad_context* context) const noexcept {
    whisper_vad_free(context);
  }
};

using WhisperVadContext =
    std::unique_ptr<whisper_vad_context, WhisperVadContextDeleter>;

struct WhisperVadSegmentsDeleter {
  void operator()(whisper_vad_segments* segments) const noexcept {
    whisper_vad_free_segments(segments);
  }
};

using WhisperVadSegments =
    std::unique_ptr<whisper_vad_segments, WhisperVadSegmentsDeleter>;

std::int64_t samples_to_centiseconds(int samples) {
  return static_cast<std::int64_t>(std::lround(
      static_cast<double>(samples) * kWhisperCentisecondsPerSecond /
      WHISPER_SAMPLE_RATE));
}

int centiseconds_to_samples(std::int64_t centiseconds) {
  return static_cast<int>(std::lround(
      static_cast<double>(centiseconds) * WHISPER_SAMPLE_RATE /
      kWhisperCentisecondsPerSecond));
}

void append_vad_mapping_point(VadTimeMapper& mapper,
                              std::int64_t processed_centiseconds,
                              std::int64_t original_centiseconds) {
  if (!mapper.points.empty() &&
      mapper.points.back().processed_centiseconds == processed_centiseconds) {
    mapper.points.back().original_centiseconds = original_centiseconds;
    return;
  }
  mapper.points.push_back({processed_centiseconds, original_centiseconds});
}

VadTimeMapper build_vad_time_mapper(const std::vector<float>& samples,
                                    whisper_vad_segments* segments,
                                    const whisper_vad_params& vad_params) {
  VadTimeMapper mapper;
  if (samples.empty()) return mapper;

  const int segment_count = whisper_vad_segments_n_segments(segments);
  const int overlap_samples = static_cast<int>(
      vad_params.samples_overlap * WHISPER_SAMPLE_RATE);
  const int silence_samples = static_cast<int>(
      kWhisperVadProcessedGapMilliseconds * WHISPER_SAMPLE_RATE / 1000);
  int processed_offset_samples = 0;
  const bool debug = std::getenv("SVP_VAD_DEBUG") != nullptr;

  for (int index = 0; index < segment_count; ++index) {
    const std::int64_t original_start_centiseconds = static_cast<std::int64_t>(
        std::llround(whisper_vad_segments_get_segment_t0(segments, index)));
    const std::int64_t original_end_centiseconds = static_cast<std::int64_t>(
        std::llround(whisper_vad_segments_get_segment_t1(segments, index)));
    int segment_start_samples =
        centiseconds_to_samples(original_start_centiseconds);
    int segment_end_samples = centiseconds_to_samples(original_end_centiseconds);
    segment_start_samples = std::clamp(segment_start_samples, 0,
                                       static_cast<int>(samples.size()) - 1);
    segment_end_samples = std::clamp(segment_end_samples, 0,
                                     static_cast<int>(samples.size()) - 1);
    const int original_segment_length =
        segment_end_samples - segment_start_samples;
    // whisper.cpp packs a non-final segment's overlap extension into the
    // processed buffer too, so the copied length decides the skip — a
    // degenerate segment still contributes its overlap and silence gap.
    int copied_segment_end_samples = segment_end_samples;
    if (index < segment_count - 1) {
      copied_segment_end_samples = std::min(
          copied_segment_end_samples + overlap_samples,
          static_cast<int>(samples.size()) - 1);
    }
    const int segment_length =
        copied_segment_end_samples - segment_start_samples;
    if (segment_length <= 0) continue;
    if (debug) {
      std::fprintf(stderr,
                   "vad seg %d: orig %.2f-%.2f processed_from %.2f\n", index,
                   original_start_centiseconds / 100.0,
                   original_end_centiseconds / 100.0,
                   samples_to_centiseconds(processed_offset_samples) / 100.0);
    }

    const std::int64_t processed_segment_start =
        samples_to_centiseconds(processed_offset_samples);
    const std::int64_t processed_segment_end = samples_to_centiseconds(
        processed_offset_samples + original_segment_length);
    mapper.processed_speech_ranges.emplace_back(
        processed_segment_start, processed_segment_end);
    append_vad_mapping_point(
        mapper, processed_segment_start, original_start_centiseconds);
    append_vad_mapping_point(
        mapper, processed_segment_end, original_end_centiseconds);

    processed_offset_samples += segment_length;

    if (index < segment_count - 1) {
      append_vad_mapping_point(
          mapper, samples_to_centiseconds(processed_offset_samples),
          original_end_centiseconds);
      append_vad_mapping_point(
          mapper,
          samples_to_centiseconds(processed_offset_samples + silence_samples),
          static_cast<std::int64_t>(std::llround(
              whisper_vad_segments_get_segment_t0(segments, index + 1))));
      processed_offset_samples += silence_samples;
    }
  }

  return mapper;
}

// Mirrors the private cs_to_samples helper of whisper.cpp 1.8.6 so the
// packed speech buffer below matches its whisper_vad() sample for sample.
int whisper_vad_centiseconds_to_samples(std::int64_t centiseconds) {
  return static_cast<int>((static_cast<double>(centiseconds) /
                           kWhisperCentisecondsPerSecond) *
                              WHISPER_SAMPLE_RATE +
                          0.5);
}

std::int64_t vad_segment_centiseconds(float centiseconds) {
  return static_cast<std::int64_t>(std::llround(centiseconds));
}

// Builds the speech-only buffer that whisper_full() decodes when its
// built-in VAD is enabled, reproducing whisper.cpp 1.8.6 whisper_vad():
// each speech segment is copied, a non-final segment is extended by the VAD
// overlap, a fixed silence gap separates segments, and the buffer keeps
// whisper_vad()'s precomputed length (zero-filled past the copied audio).
// whisper_full_with_state() runs no VAD itself, so SVP packs the buffer and
// decodes it on a per-chunk state. Empty when VAD found no speech.
std::vector<float> pack_vad_speech_samples(const std::vector<float>& samples,
                                           whisper_vad_segments* segments,
                                           const whisper_vad_params& vad_params) {
  const int segment_count = whisper_vad_segments_n_segments(segments);
  if (samples.empty() || segment_count == 0) return {};
  const int sample_count = static_cast<int>(samples.size());
  const int overlap_samples = static_cast<int>(
      vad_params.samples_overlap * WHISPER_SAMPLE_RATE);
  const int silence_samples = static_cast<int>(
      kWhisperVadProcessedGapMilliseconds * WHISPER_SAMPLE_RATE / 1000);

  std::int64_t packed_length = 0;
  for (int index = 0; index < segment_count; ++index) {
    const int start = whisper_vad_centiseconds_to_samples(
        vad_segment_centiseconds(
            whisper_vad_segments_get_segment_t0(segments, index)));
    int end = whisper_vad_centiseconds_to_samples(vad_segment_centiseconds(
        whisper_vad_segments_get_segment_t1(segments, index)));
    if (index < segment_count - 1) end += overlap_samples;
    end = std::min(end, sample_count - 1);
    packed_length += end - start;
  }
  packed_length += static_cast<std::int64_t>(segment_count - 1) *
                   silence_samples;
  std::vector<float> packed(
      static_cast<std::size_t>(std::max<std::int64_t>(packed_length, 0)),
      0.0F);

  std::size_t offset = 0;
  for (int index = 0; index < segment_count; ++index) {
    const int start = std::min(
        whisper_vad_centiseconds_to_samples(vad_segment_centiseconds(
            whisper_vad_segments_get_segment_t0(segments, index))),
        sample_count - 1);
    int end = std::min(
        whisper_vad_centiseconds_to_samples(vad_segment_centiseconds(
            whisper_vad_segments_get_segment_t1(segments, index))),
        sample_count - 1);
    if (index < segment_count - 1) {
      end = std::min(end + overlap_samples, sample_count - 1);
    }
    const int length = end - start;
    if (length <= 0) continue;
    const std::size_t gap =
        index < segment_count - 1 ? static_cast<std::size_t>(silence_samples)
                                  : 0;
    // whisper_vad() would write past its buffer here; grow instead.
    if (offset + static_cast<std::size_t>(length) + gap > packed.size()) {
      packed.resize(offset + static_cast<std::size_t>(length) + gap, 0.0F);
    }
    std::copy_n(samples.begin() + start, length, packed.begin() + offset);
    offset += static_cast<std::size_t>(length) + gap;
  }
  return packed;
}

}  // namespace

bool VadTimeMapper::same_speech_region(std::int64_t first_centiseconds,
                                       std::int64_t second_centiseconds) const {
  if (processed_speech_ranges.empty()) return false;
  const std::int64_t start = std::min(first_centiseconds, second_centiseconds);
  const std::int64_t end = std::max(first_centiseconds, second_centiseconds);
  return std::any_of(
      processed_speech_ranges.begin(), processed_speech_ranges.end(),
      [start, end](const auto& range) {
        return start >= range.first && end <= range.second;
      });
}

std::optional<std::int64_t> VadTimeMapper::speech_region_start(
    std::int64_t processed_centiseconds) const {
  for (const auto& range : processed_speech_ranges) {
    if (processed_centiseconds >= range.first &&
        processed_centiseconds <= range.second) {
      return range.first;
    }
  }
  return std::nullopt;
}

std::int64_t VadTimeMapper::map_centiseconds(std::int64_t processed_centiseconds,
                                             bool is_onset) const {
  if (points.empty()) return processed_centiseconds;
  if (processed_centiseconds <= points.front().processed_centiseconds) {
    return points.front().original_centiseconds;
  }

  for (std::size_t index = 1; index < points.size(); ++index) {
    const VadTimeMappingPoint& previous = points[index - 1];
    const VadTimeMappingPoint& current = points[index];
    if (processed_centiseconds > current.processed_centiseconds) continue;
    const std::int64_t processed_span =
        current.processed_centiseconds - previous.processed_centiseconds;
    if (processed_span <= 0) return current.original_centiseconds;
    const std::int64_t original_span =
        current.original_centiseconds - previous.original_centiseconds;
    // original time advancing far beyond processed time marks a synthetic
    // gap (removed silence); a small slack absorbs centisecond rounding.
    if (original_span > processed_span + kVadSyntheticGapSlackCentiseconds) {
      return is_onset ? current.original_centiseconds
                      : previous.original_centiseconds;
    }
    return previous.original_centiseconds +
           ((processed_centiseconds - previous.processed_centiseconds) *
            original_span) /
               processed_span;
  }
  return points.back().original_centiseconds;
}

VadSpeechInput detect_vad_speech(const std::filesystem::path& vad_model_path,
                                 const std::vector<float>& samples,
                                 const whisper_vad_params& vad_params,
                                 int vad_threads) {
  VadSpeechInput input;
  if (samples.empty()) return input;

  whisper_vad_context_params context_params =
      whisper_vad_default_context_params();
  context_params.n_threads = vad_threads;
  const std::string vad_model_path_string = vad_model_path.string();
  WhisperVadContext vad_context(
      whisper_vad_init_from_file_with_params(
          vad_model_path_string.c_str(), context_params));
  if (!vad_context) {
    throw std::runtime_error("Unable to initialize whisper.cpp VAD model: " +
                             vad_model_path.string());
  }

  WhisperVadSegments segments(whisper_vad_segments_from_samples(
      vad_context.get(), vad_params, samples.data(),
      static_cast<int>(samples.size())));
  if (!segments) {
    throw std::runtime_error("Unable to detect speech with whisper.cpp VAD");
  }

  input.time_mapper =
      build_vad_time_mapper(samples, segments.get(), vad_params);
  input.speech_samples =
      pack_vad_speech_samples(samples, segments.get(), vad_params);
  return input;
}

}  // namespace svp::audio

#endif
