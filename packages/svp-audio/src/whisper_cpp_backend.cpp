#include "svp/audio/whisper_cpp_backend.hpp"

#include "svp/audio/ctc_forced_aligner.hpp"
#include "svp/audio/phoneme_lexicon.hpp"
#include "svp/audio/whisper_pcm_reader.hpp"
#include "svp/audio/word_boundary_conversion.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
#include <whisper.h>
#endif

namespace svp::audio {

#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
namespace {

constexpr unsigned int kMaximumInferenceThreads = 8;
constexpr std::int64_t kWhisperCentisecondsPerSecond = 100;
constexpr std::int64_t kWhisperCentisecondsToMicroseconds = 10'000;
constexpr std::int64_t kWhisperVadProcessedGapMilliseconds = 100;

// whisper_model_type exposes whisper.cpp's private e_model ordinal through its
// public C API. Keep these values named so the DTW preset mapping is explicit.
enum class WhisperModelType : int {
  kUnknown = 0,
  kTiny = 1,
  kBase = 2,
  kSmall = 3,
  kMedium = 4,
  kLarge = 5,
};

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

struct VadTimeMappingPoint {
  std::int64_t processed_centiseconds = 0;
  std::int64_t original_centiseconds = 0;
};

struct VadTimeMapper {
  std::vector<VadTimeMappingPoint> points;
  std::vector<std::pair<std::int64_t, std::int64_t>> processed_speech_ranges;

  [[nodiscard]] bool same_speech_region(
      std::int64_t first_centiseconds,
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

  [[nodiscard]] std::optional<std::int64_t> speech_region_start(
      std::int64_t processed_centiseconds) const {
    for (const auto& range : processed_speech_ranges) {
      if (processed_centiseconds >= range.first &&
          processed_centiseconds <= range.second) {
        return range.first;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::int64_t map_centiseconds(
      std::int64_t processed_centiseconds) const {
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
      return previous.original_centiseconds +
             ((processed_centiseconds - previous.processed_centiseconds) *
              original_span) /
                 processed_span;
    }
    return points.back().original_centiseconds;
  }
};

std::atomic<bool> g_whisper_cpp_verbose{false};

void whisper_cpp_log_callback(ggml_log_level level,
                              const char* message,
                              void*) {
  if (message == nullptr) return;
  if (g_whisper_cpp_verbose.load(std::memory_order_relaxed) ||
      level == GGML_LOG_LEVEL_ERROR) {
    std::fputs(message, stderr);
  }
}

struct WhisperContextDeleter {
  void operator()(whisper_context* context) const noexcept {
    whisper_free(context);
  }
};

using WhisperContext = std::unique_ptr<whisper_context, WhisperContextDeleter>;

struct CachedWhisperModel {
  std::mutex mutex;
  std::filesystem::path path;
  WhisperContext context;
};

CachedWhisperModel& cached_model() {
  static CachedWhisperModel model;
  return model;
}

int inference_thread_count() {
  const unsigned int available = std::thread::hardware_concurrency();
  return static_cast<int>(std::max(
      1U, std::min(available == 0 ? 1U : available,
                   kMaximumInferenceThreads)));
}

WhisperContext load_context(const std::filesystem::path& model_path,
                            whisper_context_params params) {
  WhisperContext loaded(
      whisper_init_from_file_with_params(model_path.string().c_str(), params));
  if (!loaded) {
    throw std::runtime_error("Unable to load GGML Whisper model: " +
                             model_path.string());
  }
  return loaded;
}

whisper_alignment_heads_preset dtw_aheads_preset_for_context(
    whisper_context* context) {
  const bool multilingual = whisper_is_multilingual(context) != 0;
  switch (static_cast<WhisperModelType>(whisper_model_type(context))) {
    case WhisperModelType::kTiny:
      return multilingual ? WHISPER_AHEADS_TINY : WHISPER_AHEADS_TINY_EN;
    case WhisperModelType::kBase:
      return multilingual ? WHISPER_AHEADS_BASE : WHISPER_AHEADS_BASE_EN;
    case WhisperModelType::kSmall:
      return multilingual ? WHISPER_AHEADS_SMALL : WHISPER_AHEADS_SMALL_EN;
    case WhisperModelType::kMedium:
      return multilingual ? WHISPER_AHEADS_MEDIUM : WHISPER_AHEADS_MEDIUM_EN;
    case WhisperModelType::kLarge:
      throw std::runtime_error(
          "Cannot derive a version-specific DTW alignment preset for a large "
          "Whisper model from the vendored whisper.cpp API");
    case WhisperModelType::kUnknown:
      break;
  }

  throw std::runtime_error("Cannot derive a DTW alignment preset for Whisper model type " +
                           std::to_string(whisper_model_type(context)));
}

whisper_context* load_model(CachedWhisperModel& cache,
                            const std::filesystem::path& model_path) {
  if (cache.context && cache.path == model_path) return cache.context.get();

  whisper_context_params inspect_params = whisper_context_default_params();
  // GPU use is a preference. whisper.cpp retains its CPU backend when the
  // platform build has no supported accelerator.
  inspect_params.use_gpu = true;
  WhisperContext inspected = load_context(model_path, inspect_params);
  const whisper_alignment_heads_preset dtw_aheads_preset =
      dtw_aheads_preset_for_context(inspected.get());

  whisper_context_params params = whisper_context_default_params();
  params.use_gpu = true;
  // whisper.cpp silently disables DTW when flash attention is enabled because
  // DTW needs the explicit cross-attention weights.
  params.flash_attn = false;
  params.dtw_token_timestamps = true;
  params.dtw_aheads_preset = dtw_aheads_preset;
  WhisperContext loaded = load_context(model_path, params);
  cache.path = model_path;
  cache.context = std::move(loaded);
  return cache.context.get();
}

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

VadTimeMapper build_vad_time_mapper(
    const std::filesystem::path& vad_model_path,
    const std::vector<float>& samples,
    whisper_vad_params vad_params) {
  VadTimeMapper mapper;
  if (samples.empty()) return mapper;

  whisper_vad_context_params context_params =
      whisper_vad_default_context_params();
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

  const int segment_count = whisper_vad_segments_n_segments(segments.get());
  const int overlap_samples = static_cast<int>(
      vad_params.samples_overlap * WHISPER_SAMPLE_RATE);
  const int silence_samples = static_cast<int>(
      kWhisperVadProcessedGapMilliseconds * WHISPER_SAMPLE_RATE / 1000);
  int processed_offset_samples = 0;

  for (int index = 0; index < segment_count; ++index) {
    const std::int64_t original_start_centiseconds = static_cast<std::int64_t>(
        std::llround(whisper_vad_segments_get_segment_t0(segments.get(), index)));
    const std::int64_t original_end_centiseconds = static_cast<std::int64_t>(
        std::llround(whisper_vad_segments_get_segment_t1(segments.get(), index)));
    int segment_start_samples =
        centiseconds_to_samples(original_start_centiseconds);
    int segment_end_samples = centiseconds_to_samples(original_end_centiseconds);
    segment_start_samples = std::clamp(segment_start_samples, 0,
                                       static_cast<int>(samples.size()) - 1);
    segment_end_samples = std::clamp(segment_end_samples, 0,
                                     static_cast<int>(samples.size()) - 1);
    const int original_segment_length =
        segment_end_samples - segment_start_samples;
    if (original_segment_length <= 0) continue;

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

    int copied_segment_end_samples = segment_end_samples;
    if (index < segment_count - 1) {
      copied_segment_end_samples = std::min(
          copied_segment_end_samples + overlap_samples,
          static_cast<int>(samples.size()) - 1);
    }
    processed_offset_samples += copied_segment_end_samples - segment_start_samples;

    if (index < segment_count - 1) {
      append_vad_mapping_point(
          mapper, samples_to_centiseconds(processed_offset_samples),
          original_end_centiseconds);
      append_vad_mapping_point(
          mapper,
          samples_to_centiseconds(processed_offset_samples + silence_samples),
          static_cast<std::int64_t>(std::llround(
              whisper_vad_segments_get_segment_t0(segments.get(), index + 1))));
      processed_offset_samples += silence_samples;
    }
  }

  return mapper;
}

std::int64_t clamped_token_time_us(
    std::int64_t centiseconds,
    const VadTimeMapper& vad_time_mapper,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us) {
  if (centiseconds < 0) return chunk_start_us;
  const std::int64_t original_centiseconds =
      vad_time_mapper.map_centiseconds(centiseconds);
  return std::clamp<std::int64_t>(
      chunk_start_us + original_centiseconds *
          kWhisperCentisecondsToMicroseconds,
      chunk_start_us, chunk_end_us);
}

std::int64_t token_start_us(const whisper_token_data& token_data,
                            const VadTimeMapper& vad_time_mapper,
                            std::int64_t chunk_start_us,
                            std::int64_t chunk_end_us) {
  const bool dtw_onset_is_valid =
      token_data.t_dtw >= 0 &&
      (token_data.t1 < 0 || token_data.t_dtw < token_data.t1);
  const bool decoder_onset_is_same_speech_region =
      token_data.t0 >= 0 && token_data.t0 <= token_data.t_dtw &&
      vad_time_mapper.same_speech_region(token_data.t0, token_data.t_dtw);
  const std::optional<std::int64_t> speech_region_start =
      token_data.t_dtw >= 0
          ? vad_time_mapper.speech_region_start(token_data.t_dtw)
          : std::nullopt;
  const std::int64_t onset_centiseconds =
      !dtw_onset_is_valid
          ? (token_data.t0 >= 0 ? token_data.t0 : token_data.t1)
          : (decoder_onset_is_same_speech_region
                 ? token_data.t0
                 : (token_data.t0 >= 0 && token_data.t0 < token_data.t_dtw &&
                            speech_region_start.has_value()
                        ? *speech_region_start
                        : token_data.t_dtw));
  return clamped_token_time_us(onset_centiseconds, vad_time_mapper,
                               chunk_start_us, chunk_end_us);
}

std::int64_t token_end_us(const whisper_token_data& token_data,
                          const VadTimeMapper& vad_time_mapper,
                          std::int64_t chunk_start_us,
                          std::int64_t chunk_end_us) {
  return clamped_token_time_us(token_data.t1, vad_time_mapper,
                               chunk_start_us, chunk_end_us);
}

std::string trim_leading_space(const char* raw) {
  std::string text = raw == nullptr ? std::string{} : std::string(raw);
  const auto first = std::find_if_not(
      text.begin(), text.end(),
      [](unsigned char character) { return std::isspace(character) != 0; });
  text.erase(text.begin(), first);
  return text;
}

std::vector<AsrWord> collect_segment_words(
    whisper_context* context,
    int segment_index,
    const VadTimeMapper& vad_time_mapper,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us) {
  std::vector<AsrWord> words;
  AsrWord current;
  double probability_sum = 0.0;
  std::size_t probability_count = 0;

  auto flush = [&](std::optional<std::int64_t> next_word_start_us = std::nullopt) {
    if (current.text.empty()) return;
    if (next_word_start_us.has_value()) {
      current.end_us = std::min(current.end_us, *next_word_start_us);
    }
    current.confidence = probability_count == 0
                             ? 0.0
                             : probability_sum /
                                   static_cast<double>(probability_count);
    if (current.end_us > current.start_us) words.push_back(current);
    current = AsrWord{};
    probability_sum = 0.0;
    probability_count = 0;
  };

  const int token_count = whisper_full_n_tokens(context, segment_index);
  for (int token_index = 0; token_index < token_count; ++token_index) {
    const whisper_token token_id =
        whisper_full_get_token_id(context, segment_index, token_index);
    if (token_id >= whisper_token_eot(context)) continue;

    const char* raw_text =
        whisper_full_get_token_text(context, segment_index, token_index);
    if (raw_text == nullptr || *raw_text == '\0') continue;
    const bool begins_word = std::isspace(
                                 static_cast<unsigned char>(*raw_text)) != 0;
    const std::string token_text = trim_leading_space(raw_text);
    if (token_text.empty()) continue;

    const whisper_token_data token_data =
        whisper_full_get_token_data(context, segment_index, token_index);
    const std::int64_t current_token_start_us = token_start_us(
        token_data, vad_time_mapper, chunk_start_us, chunk_end_us);
    if (begins_word && !current.text.empty()) {
      flush(current_token_start_us);
    }

    const std::int64_t current_token_end_us = token_end_us(
        token_data, vad_time_mapper, chunk_start_us, chunk_end_us);
    if (current.text.empty()) current.start_us = current_token_start_us;
    current.text += token_text;
    current.end_us = std::max(current.end_us, current_token_end_us);
    probability_sum +=
        whisper_full_get_token_p(context, segment_index, token_index);
    ++probability_count;
  }
  flush();
  return words;
}

void clamp_word_ends_to_next_onset(std::vector<AsrWord>& words) {
  for (std::size_t index = 0; index + 1 < words.size(); ++index) {
    words[index].end_us = std::min(words[index].end_us,
                                   words[index + 1].start_us);
  }
  words.erase(
      std::remove_if(words.begin(), words.end(),
                     [](const AsrWord& word) {
                       return word.end_us <= word.start_us;
                     }),
      words.end());
}

struct CachedPhonemeAligner {
  std::mutex mutex;
  std::filesystem::path path;
  std::optional<CtcForcedAligner> aligner;
  std::optional<PhonemeLexicon> lexicon;
};

CachedPhonemeAligner& cached_aligner() {
  static CachedPhonemeAligner cache;
  return cache;
}

std::int64_t seconds_to_chunk_us(double seconds, std::int64_t chunk_start_us,
                                 std::int64_t chunk_end_us) {
  return std::clamp<std::int64_t>(
      chunk_start_us + static_cast<std::int64_t>(std::llround(seconds * 1e6)),
      chunk_start_us, chunk_end_us);
}

// Replaces whisper.cpp token-derived word starts with CTC forced-alignment
// starts converted to the NLE packed-word convention. Whisper still owns
// the transcript text; alignment only assigns times. Words the lexicon
// cannot resolve, and any alignment failure, keep whisper timing.
void apply_phoneme_alignment(WhisperInferenceResult& result,
                             const std::vector<float>& samples,
                             const std::filesystem::path& bundle_dir,
                             std::int64_t chunk_start_us,
                             std::int64_t chunk_end_us) {
  std::vector<AsrWord>& words = result.all_words;
  if (words.empty()) return;

  CachedPhonemeAligner& cache = cached_aligner();
  std::scoped_lock lock(cache.mutex);
  if (!cache.aligner.has_value() || cache.path != bundle_dir) {
    PhonemeLexicon lexicon = PhonemeLexicon::load_from_bundle(bundle_dir);
    CtcForcedAligner aligner = CtcForcedAligner::load(bundle_dir);
    cache.lexicon = std::move(lexicon);
    cache.aligner = std::move(aligner);
    cache.path = bundle_dir;
  }

  std::vector<CtcAlignmentToken> tokens;
  std::vector<bool> resolvable(words.size(), false);
  for (std::size_t i = 0; i < words.size(); ++i) {
    const std::string normalized =
        normalize_word_for_lexicon(words[i].text);
    if (normalized.empty()) continue;
    const std::optional<std::vector<std::string>> phones =
        cache.lexicon->phones_for(normalized);
    if (!phones.has_value() || phones->empty()) continue;
    for (const std::string& phone : *phones) {
      tokens.push_back({phone, i});
    }
    resolvable[i] = true;
  }
  if (tokens.empty()) {
    result.alignment_status = "fallback";
    return;
  }

  const std::vector<CtcPhoneSpan> phone_spans =
      cache.aligner->align(samples, tokens);

  std::vector<AlignedWordSpan> spans(words.size());
  std::vector<bool> has_span(words.size(), false);
  for (const CtcPhoneSpan& span : phone_spans) {
    const std::size_t owner = tokens[span.token_index].word_index;
    AlignedWordSpan& word = spans[owner];
    if (!has_span[owner]) {
      word.start_seconds = span.start_seconds;
      word.end_seconds = span.end_seconds;
      has_span[owner] = true;
    } else {
      word.start_seconds = std::min(word.start_seconds, span.start_seconds);
      word.end_seconds = std::max(word.end_seconds, span.end_seconds);
    }
  }
  // Unresolved words contribute their whisper interval so gap and phone
  // context stay defined for their aligned neighbours; their own timing
  // is never overwritten below.
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (has_span[i]) continue;
    spans[i].start_seconds =
        static_cast<double>(words[i].start_us - chunk_start_us) / 1e6;
    spans[i].end_seconds =
        static_cast<double>(words[i].end_us - chunk_start_us) / 1e6;
  }

  const std::vector<double> converted = convert_aligned_word_starts(
      spans, phone_spans, tokens, samples);

  std::size_t applied = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    // A converted start is only meaningful between two aligned words;
    // word zero has no left context and applies directly.
    if (!has_span[i] || (i > 0 && !has_span[i - 1])) continue;
    words[i].start_us =
        seconds_to_chunk_us(converted[i], chunk_start_us, chunk_end_us);
    words[i].end_us = std::min(
        seconds_to_chunk_us(spans[i].end_seconds, chunk_start_us,
                            chunk_end_us),
        i + 1 < words.size() ? words[i + 1].start_us : chunk_end_us);
    words[i].end_us = std::max(words[i].end_us, words[i].start_us);
    ++applied;
  }
  for (std::size_t i = 0; i + 1 < words.size(); ++i) {
    if (words[i].end_us > words[i + 1].start_us) {
      words[i].end_us = words[i + 1].start_us;
    }
  }
  result.alignment_status =
      applied == words.size() ? "applied" : "applied_partial";
}

}  // namespace
#endif

bool is_whisper_cpp_runtime_available() noexcept {
#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
  return true;
#else
  return false;
#endif
}

void set_whisper_cpp_verbose(bool verbose) noexcept {
#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
  g_whisper_cpp_verbose.store(verbose, std::memory_order_relaxed);
  whisper_log_set(whisper_cpp_log_callback, nullptr);
  ggml_log_set(whisper_cpp_log_callback, nullptr);
#else
  (void)verbose;
#endif
}

void release_whisper_cpp_model() noexcept {
#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
  CachedWhisperModel& cache = cached_model();
  std::scoped_lock lock(cache.mutex);
  cache.context.reset();
  cache.path.clear();
#endif
}

void release_phoneme_aligner() noexcept {
#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
  CachedPhonemeAligner& cache = cached_aligner();
  std::scoped_lock lock(cache.mutex);
  cache.aligner.reset();
  cache.lexicon.reset();
  cache.path.clear();
#endif
}

WhisperInferenceResult run_whisper_cpp_inference(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& ggml_model_path,
    const std::filesystem::path& vad_model_path,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us,
    const std::filesystem::path& aligner_bundle_dir) {
#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
  WhisperInferenceResult result;
  const std::vector<float> samples = read_whisper_pcm16_mono_wav(wav_path);
  if (samples.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("Whisper input exceeds the runtime sample limit");
  }

  CachedWhisperModel& cache = cached_model();
  std::scoped_lock lock(cache.mutex);
  whisper_context* context = load_model(cache, ggml_model_path);

  whisper_full_params params =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.n_threads = inference_thread_count();
  params.language = "en";
  params.translate = false;
  params.no_context = true;
  params.no_timestamps = false;
  params.token_timestamps = true;
  params.split_on_word = true;
  params.print_progress = false;
  params.print_realtime = false;
  params.print_timestamps = false;
  params.print_special = false;
  params.vad = true;
  const std::string vad_model_path_string = vad_model_path.string();
  params.vad_model_path = vad_model_path_string.c_str();
  params.vad_params = whisper_vad_default_params();
  const VadTimeMapper vad_time_mapper = build_vad_time_mapper(
      vad_model_path, samples, params.vad_params);

  if (whisper_full(context, params, samples.data(),
                   static_cast<int>(samples.size())) != 0) {
    throw std::runtime_error("whisper.cpp inference failed");
  }

  const int segment_count = whisper_full_n_segments(context);
  for (int segment_index = 0; segment_index < segment_count; ++segment_index) {
    WhisperSegment segment;
    const char* text = whisper_full_get_segment_text(context, segment_index);
    if (text != nullptr) segment.text = text;
    segment.start_us = clamped_token_time_us(
        whisper_full_get_segment_t0(context, segment_index), vad_time_mapper,
        chunk_start_us, chunk_end_us);
    segment.end_us = clamped_token_time_us(
        whisper_full_get_segment_t1(context, segment_index), vad_time_mapper,
        chunk_start_us, chunk_end_us);
    segment.words = collect_segment_words(
        context, segment_index, vad_time_mapper, chunk_start_us, chunk_end_us);
    result.all_words.insert(result.all_words.end(), segment.words.begin(),
                            segment.words.end());
    result.segments.push_back(std::move(segment));
  }
  clamp_word_ends_to_next_onset(result.all_words);
  if (!aligner_bundle_dir.empty()) {
    try {
      apply_phoneme_alignment(result, samples, aligner_bundle_dir,
                              chunk_start_us, chunk_end_us);
    } catch (const std::exception&) {
      result.alignment_status = "fallback";
    }
  }
  result.ran = true;
  result.termination_reason = "end_of_transcript";
  return result;
#else
  (void)wav_path;
  (void)ggml_model_path;
  (void)vad_model_path;
  (void)aligner_bundle_dir;
  (void)chunk_start_us;
  (void)chunk_end_us;
  WhisperInferenceResult result;
  result.blockers.push_back("whisper.cpp is not available in this build");
  return result;
#endif
}

}  // namespace svp::audio
