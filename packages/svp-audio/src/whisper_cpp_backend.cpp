#include "svp/audio/whisper_cpp_backend.hpp"

#include "svp/audio/ctc_forced_aligner.hpp"
#include "svp/audio/phoneme_lexicon.hpp"
#include "svp/audio/whisper_pcm_reader.hpp"
#include "svp/audio/word_boundary_conversion.hpp"
#include "whisper_vad_speech.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
#include <ggml-backend.h>
#include <whisper.h>
#endif

namespace svp::audio {

#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
namespace {

constexpr std::int64_t kWhisperCentisecondsToMicroseconds = 10'000;

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

struct WhisperStateDeleter {
  void operator()(whisper_state* state) const noexcept {
    whisper_free_state(state);
  }
};

// Decoding state for exactly one chunk. whisper.cpp keeps mutable decode
// state here (KV caches regrown when temperature fallback adds decoders,
// backend compute buffers, the first decoder's sampling RNG, the previous
// result), so a state shared across chunks makes each chunk's output depend
// on the chunks decoded before it. A fresh state per chunk makes every chunk
// a pure function of its own samples; the cached context holds only the
// immutable model weights.
using WhisperState = std::unique_ptr<whisper_state, WhisperStateDeleter>;

struct CachedWhisperModel {
  std::mutex mutex;
  std::filesystem::path path;
  WhisperContext context;
};

// The cached context lives until release_whisper_cpp_model() or process exit.
// With the Metal backend its weights are Metal buffers, each registered in a
// residency-set collection owned by ggml's Metal device. ggml keeps those
// devices in function-local statics created the first time its backend
// registry is used, and destroying a device asserts that every buffer was
// already freed (ggml_metal_rsets_free). Objects with static storage duration
// are destroyed in the reverse order of their construction, so a cache
// constructed before ggml's devices outlives them at exit: any process that
// exits with a model still cached aborts instead of exiting. Initializing the
// backend registry before constructing the cache makes the cache, and the
// buffers it still holds, go first, whatever path led to exit.
CachedWhisperModel& cached_model() {
  [[maybe_unused]] static const std::size_t ggml_devices_constructed_first =
      ggml_backend_dev_count();
  static CachedWhisperModel model;
  return model;
}

WhisperContext load_context(const std::filesystem::path& model_path,
                            whisper_context_params params) {
  // The context is shared across chunks, so it carries no decoding state of
  // its own; each chunk allocates a fresh whisper_state instead.
  WhisperContext loaded(whisper_init_from_file_with_params_no_state(
      model_path.string().c_str(), params));
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

std::int64_t clamped_token_time_us(
    std::int64_t centiseconds,
    const VadTimeMapper& vad_time_mapper,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us,
    bool is_onset) {
  if (centiseconds < 0) return chunk_start_us;
  const std::int64_t original_centiseconds =
      vad_time_mapper.map_centiseconds(centiseconds, is_onset);
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
                               chunk_start_us, chunk_end_us, true);
}

std::int64_t token_end_us(const whisper_token_data& token_data,
                          const VadTimeMapper& vad_time_mapper,
                          std::int64_t chunk_start_us,
                          std::int64_t chunk_end_us) {
  return clamped_token_time_us(token_data.t1, vad_time_mapper,
                               chunk_start_us, chunk_end_us, false);
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
    whisper_state* state,
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

  const int token_count =
      whisper_full_n_tokens_from_state(state, segment_index);
  for (int token_index = 0; token_index < token_count; ++token_index) {
    const whisper_token token_id =
        whisper_full_get_token_id_from_state(state, segment_index, token_index);
    if (token_id >= whisper_token_eot(context)) continue;

    const char* raw_text = whisper_full_get_token_text_from_state(
        context, state, segment_index, token_index);
    if (raw_text == nullptr || *raw_text == '\0') continue;
    const bool begins_word = std::isspace(
                                 static_cast<unsigned char>(*raw_text)) != 0;
    const std::string token_text = trim_leading_space(raw_text);
    if (token_text.empty()) continue;

    const whisper_token_data token_data =
        whisper_full_get_token_data_from_state(state, segment_index,
                                               token_index);
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
        whisper_full_get_token_p_from_state(state, segment_index, token_index);
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
  svp::models::OrtThreadCounts threads;
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
                             const svp::models::OrtThreadCounts& threads,
                             std::int64_t chunk_start_us,
                             std::int64_t chunk_end_us) {
  std::vector<AsrWord>& words = result.all_words;
  if (words.empty()) return;

  CachedPhonemeAligner& cache = cached_aligner();
  std::scoped_lock lock(cache.mutex);
  if (!cache.aligner.has_value() || cache.path != bundle_dir ||
      cache.threads != threads) {
    PhonemeLexicon lexicon = PhonemeLexicon::load_from_bundle(bundle_dir);
    CtcForcedAligner aligner = CtcForcedAligner::load(bundle_dir, threads);
    cache.lexicon = std::move(lexicon);
    cache.aligner = std::move(aligner);
    cache.path = bundle_dir;
    cache.threads = threads;
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

  // The decoder's own interval end bounds the resumption scan: an aligned
  // span can over-extend into the next word's onset, and a rise past this
  // point belongs to that word, not to this one.
  std::vector<double> decoder_ends(words.size());
  for (std::size_t i = 0; i < words.size(); ++i) {
    decoder_ends[i] =
        static_cast<double>(words[i].end_us - chunk_start_us) / 1e6;
  }

  const std::vector<double> converted = convert_aligned_word_starts(
      spans, phone_spans, tokens, samples, decoder_ends);
  const std::vector<double> snapped = snap_word_starts_to_resumptions(
      spans, samples);

  // Starts are decided for every word before any end is written: a word's
  // end must clamp to its successor's final start, not to a stale decoder
  // estimate that may sit seconds early after VAD silence removal.
  std::vector<std::int64_t> final_starts(words.size());
  std::vector<bool> aligned(words.size());
  for (std::size_t i = 0; i < words.size(); ++i) {
    // A converted start is only meaningful between two aligned words;
    // word zero has no left context and applies directly.
    aligned[i] = has_span[i] && (i == 0 || has_span[i - 1]);
    if (aligned[i]) {
      final_starts[i] = seconds_to_chunk_us(converted[i], chunk_start_us,
                                          chunk_end_us);
      continue;
    }
    // A word cannot straddle a real pause even without phone alignment:
    // pull its start out of the silence inside its own interval.
    const std::int64_t snapped_start = seconds_to_chunk_us(
        snapped[i], chunk_start_us, chunk_end_us);
    final_starts[i] = words[i].start_us;
    if (snapped_start > words[i].start_us &&
        snapped_start < words[i].end_us) {
      final_starts[i] = snapped_start;
    }
  }

  std::size_t applied = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const std::int64_t next_start =
        i + 1 < words.size() ? final_starts[i + 1] : chunk_end_us;
    if (!aligned[i]) {
      words[i].start_us = final_starts[i];
      words[i].end_us = std::min(words[i].end_us, next_start);
      continue;
    }
    const std::int64_t decoder_start = words[i].start_us;
    const std::int64_t decoder_end = words[i].end_us;
    const std::int64_t end_us = std::min(
        seconds_to_chunk_us(spans[i].end_seconds, chunk_start_us,
                            chunk_end_us),
        next_start);
    if (end_us > final_starts[i]) {
      words[i].timing_source = "wav2vec2_espeak_ctc";
      words[i].start_us = final_starts[i];
      words[i].end_us = end_us;
      ++applied;
      continue;
    }
    // Conversion collapsed the interval; keep the decoder's timing so the
    // word is not silently dropped downstream, still allowing the
    // resumption snap to pull its start out of a silence inside it.
    if (std::getenv("SVP_BOUNDARY_DEBUG") != nullptr) {
      std::fprintf(stderr,
                   "collapsed w=%zu '%s' conv_start=%.3f span_end=%.3f "
                   "dec=[%.3f,%.3f] next=%.3f\n",
                   i, words[i].text.c_str(), converted[i],
                   spans[i].end_seconds,
                   (decoder_start - chunk_start_us) / 1e6,
                   (decoder_end - chunk_start_us) / 1e6,
                   (next_start - chunk_start_us) / 1e6);
    }
    words[i].start_us = decoder_start;
    words[i].end_us = decoder_end;
    const std::int64_t snapped_start = seconds_to_chunk_us(
        snapped[i], chunk_start_us, chunk_end_us);
    if (snapped_start > words[i].start_us &&
        snapped_start < words[i].end_us) {
      words[i].start_us = snapped_start;
    }
    words[i].end_us = std::min(words[i].end_us, next_start);
    words[i].timing_source = "whisper_cpp_dtw";
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
    const WhisperRuntimeThreads& threads,
    const std::filesystem::path& aligner_bundle_dir) {
#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
  if (threads.whisper.decode <= 0 || threads.whisper.vad <= 0) {
    throw std::invalid_argument(
        "whisper.cpp needs positive decode and VAD thread counts");
  }
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
  params.n_threads = threads.whisper.decode;
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
  // Speech-only decoding with whisper.cpp's default Silero VAD parameters.
  // whisper_full_with_state() has no VAD step, so detect_vad_speech() packs
  // the speech buffer exactly as whisper_full()'s built-in VAD would.
  params.vad = false;
  const whisper_vad_params vad_params = whisper_vad_default_params();
  const VadSpeechInput speech = detect_vad_speech(
      vad_model_path, samples, vad_params, threads.whisper.vad);
  const VadTimeMapper& vad_time_mapper = speech.time_mapper;

  const WhisperState state(whisper_init_state(context));
  if (!state) {
    throw std::runtime_error("Unable to allocate whisper.cpp decoding state");
  }
  // No speech: whisper_full() returns an empty result without decoding.
  if (!speech.speech_samples.empty() &&
      whisper_full_with_state(
          context, state.get(), params, speech.speech_samples.data(),
          static_cast<int>(speech.speech_samples.size())) != 0) {
    throw std::runtime_error("whisper.cpp inference failed");
  }

  const int segment_count = whisper_full_n_segments_from_state(state.get());
  for (int segment_index = 0; segment_index < segment_count; ++segment_index) {
    WhisperSegment segment;
    const char* text =
        whisper_full_get_segment_text_from_state(state.get(), segment_index);
    if (text != nullptr) segment.text = text;
    segment.start_us = clamped_token_time_us(
        whisper_full_get_segment_t0_from_state(state.get(), segment_index),
        vad_time_mapper, chunk_start_us, chunk_end_us, true);
    segment.end_us = clamped_token_time_us(
        whisper_full_get_segment_t1_from_state(state.get(), segment_index),
        vad_time_mapper, chunk_start_us, chunk_end_us, false);
    segment.words = collect_segment_words(context, state.get(), segment_index,
                                          vad_time_mapper, chunk_start_us,
                                          chunk_end_us);
    result.all_words.insert(result.all_words.end(), segment.words.begin(),
                            segment.words.end());
    result.segments.push_back(std::move(segment));
  }
  clamp_word_ends_to_next_onset(result.all_words);
  if (!aligner_bundle_dir.empty()) {
    try {
      apply_phoneme_alignment(result, samples, aligner_bundle_dir,
                              threads.forced_alignment, chunk_start_us,
                              chunk_end_us);
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
  (void)threads;
  (void)chunk_start_us;
  (void)chunk_end_us;
  WhisperInferenceResult result;
  result.blockers.push_back("whisper.cpp is not available in this build");
  return result;
#endif
}

}  // namespace svp::audio
