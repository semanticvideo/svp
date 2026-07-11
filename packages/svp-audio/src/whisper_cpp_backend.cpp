#include "svp/audio/whisper_cpp_backend.hpp"

#include "svp/audio/whisper_pcm_reader.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
#include <whisper.h>
#endif

namespace svp::audio {

#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
namespace {

constexpr unsigned int kMaximumInferenceThreads = 8;
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

whisper_context* load_model(CachedWhisperModel& cache,
                            const std::filesystem::path& model_path) {
  if (cache.context && cache.path == model_path) return cache.context.get();

  whisper_context_params params = whisper_context_default_params();
  // GPU use is a preference. whisper.cpp retains its CPU backend when the
  // platform build has no supported accelerator.
  params.use_gpu = true;
  WhisperContext loaded(
      whisper_init_from_file_with_params(model_path.string().c_str(), params));
  if (!loaded) {
    throw std::runtime_error("Unable to load GGML Whisper model: " +
                             model_path.string());
  }
  cache.path = model_path;
  cache.context = std::move(loaded);
  return cache.context.get();
}

std::string trim_leading_space(const char* raw) {
  std::string text = raw == nullptr ? std::string{} : std::string(raw);
  const auto first = std::find_if_not(
      text.begin(), text.end(),
      [](unsigned char character) { return std::isspace(character) != 0; });
  text.erase(text.begin(), first);
  return text;
}

std::vector<AsrWord> collect_segment_words(whisper_context* context,
                                           int segment_index,
                                           std::int64_t chunk_start_us,
                                           std::int64_t chunk_end_us) {
  std::vector<AsrWord> words;
  AsrWord current;
  double probability_sum = 0.0;
  std::size_t probability_count = 0;

  auto flush = [&]() {
    if (current.text.empty()) return;
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
    if (begins_word && !current.text.empty()) flush();

    const whisper_token_data token_data =
        whisper_full_get_token_data(context, segment_index, token_index);
    const std::int64_t token_start_us = std::clamp<std::int64_t>(
        chunk_start_us + token_data.t0 * 10000LL,
        chunk_start_us, chunk_end_us);
    const std::int64_t token_end_us = std::clamp<std::int64_t>(
        chunk_start_us + token_data.t1 * 10000LL,
        chunk_start_us, chunk_end_us);
    if (current.text.empty()) current.start_us = token_start_us;
    current.text += token_text;
    current.end_us = std::max(current.end_us, token_end_us);
    probability_sum +=
        whisper_full_get_token_p(context, segment_index, token_index);
    ++probability_count;
  }
  flush();
  return words;
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

WhisperInferenceResult run_whisper_cpp_inference(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& ggml_model_path,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us) {
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

  if (whisper_full(context, params, samples.data(),
                   static_cast<int>(samples.size())) != 0) {
    throw std::runtime_error("whisper.cpp inference failed");
  }

  const int segment_count = whisper_full_n_segments(context);
  for (int segment_index = 0; segment_index < segment_count; ++segment_index) {
    WhisperSegment segment;
    const char* text = whisper_full_get_segment_text(context, segment_index);
    if (text != nullptr) segment.text = text;
    segment.start_us = std::clamp<std::int64_t>(
        chunk_start_us +
            whisper_full_get_segment_t0(context, segment_index) * 10000LL,
        chunk_start_us, chunk_end_us);
    segment.end_us = std::clamp<std::int64_t>(
        chunk_start_us +
            whisper_full_get_segment_t1(context, segment_index) * 10000LL,
        chunk_start_us, chunk_end_us);
    segment.words = collect_segment_words(
        context, segment_index, chunk_start_us, chunk_end_us);
    result.all_words.insert(result.all_words.end(), segment.words.begin(),
                            segment.words.end());
    result.segments.push_back(std::move(segment));
  }
  result.ran = true;
  result.termination_reason = "end_of_transcript";
  return result;
#else
  (void)wav_path;
  (void)ggml_model_path;
  (void)chunk_start_us;
  (void)chunk_end_us;
  WhisperInferenceResult result;
  result.blockers.push_back("whisper.cpp is not available in this build");
  return result;
#endif
}

}  // namespace svp::audio
