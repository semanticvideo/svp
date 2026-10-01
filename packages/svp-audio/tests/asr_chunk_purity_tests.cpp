#include "audio_test_support.hpp"

#include "svp/audio/asr_chunk_context.hpp"
#include "svp/audio/wav_slice.hpp"
#include "svp/audio/whisper_cpp_backend.hpp"
#include "svp/audio/whisper_cpp_model.hpp"
#include "svp/audio/whisper_pcm_reader.hpp"

#include <optional>

namespace {

struct PurityChunk {
  std::string chunk_id;
  std::filesystem::path wav;
  std::int64_t duration_us = 0;
};

struct PurityModels {
  std::filesystem::path whisper_dir;
  std::filesystem::path vad_model;
  std::filesystem::path aligner_dir;
};

std::vector<PurityChunk> slice_fixture_on_chunk_grid(
    const std::filesystem::path& source_wav,
    const std::filesystem::path& slice_dir) {
  const std::size_t sample_count =
      svp::audio::read_whisper_pcm16_mono_wav(source_wav).size();
  constexpr std::int64_t kMicrosecondsPerSecond = 1000000;
  // read_whisper_pcm16_mono_wav requires the 16 kHz analysis format.
  constexpr std::int64_t kAnalysisSampleRate = 16000;
  const std::int64_t duration_us =
      static_cast<std::int64_t>(sample_count) * kMicrosecondsPerSecond /
      kAnalysisSampleRate;
  const svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(duration_us);

  std::vector<PurityChunk> chunks;
  for (const svp::audio::AsrChunkPlan& chunk : plan.chunks) {
    const svp::audio::AsrChunkContextPlan context =
        svp::audio::plan_asr_chunk_context(chunk);
    chunks.push_back({chunk.chunk_id,
                      svp::audio::slice_wav_to_temp(
                          source_wav, context.slice_start_us,
                          context.slice_end_us, slice_dir),
                      context.slice_end_us - context.slice_start_us});
  }
  return chunks;
}

// Writes a transformed copy of a chunk. The transforms below produce audio
// that VAD still passes but whisper decodes with low log-probability, which
// drives it into temperature fallback (extra sampling decoders, a KV cache
// regrown for them). That decoder state used to persist into the next chunk.
template <typename Transform>
PurityChunk write_transformed_chunk(const PurityChunk& chunk,
                                    const std::string& suffix,
                                    const std::filesystem::path& slice_dir,
                                    Transform transform) {
  const std::vector<float> samples =
      transform(svp::audio::read_whisper_pcm16_mono_wav(chunk.wav));
  // Inverts read_whisper_pcm16_mono_wav's PCM16 normalization.
  constexpr float kPcm16FullScale = 32768.0F;
  constexpr long kPcm16Min = -32768;
  constexpr long kPcm16Max = 32767;
  std::vector<std::int16_t> pcm;
  pcm.reserve(samples.size());
  for (const float sample : samples) {
    pcm.push_back(static_cast<std::int16_t>(std::clamp(
        std::lround(sample * kPcm16FullScale), kPcm16Min, kPcm16Max)));
  }
  PurityChunk result{chunk.chunk_id + suffix,
                     slice_dir / (chunk.chunk_id + suffix + ".wav"),
                     chunk.duration_us};
  write_pcm_s16le_mono_wav(result.wav, pcm);
  return result;
}

std::vector<float> time_reversed(std::vector<float> samples) {
  std::reverse(samples.begin(), samples.end());
  return samples;
}

// Adds deterministic uniform noise (a fixed-seed LCG, so the test input is
// reproducible) loud enough to make the decoder uncertain.
std::vector<float> with_uniform_noise(std::vector<float> samples) {
  constexpr std::uint32_t kSeed = 12345;
  constexpr std::uint32_t kLcgMultiplier = 1664525U;
  constexpr std::uint32_t kLcgIncrement = 1013904223U;
  constexpr float kNoisePeakToPeak = 0.25F;
  constexpr float kUnitScale = 1.0F / 16777216.0F;  // top 24 LCG bits
  std::uint32_t state = kSeed;
  for (float& sample : samples) {
    state = state * kLcgMultiplier + kLcgIncrement;
    sample += kNoisePeakToPeak *
              (static_cast<float>(state >> 8) * kUnitScale - 0.5F);
  }
  return samples;
}

svp::audio::WhisperInferenceResult infer_chunk(const PurityChunk& chunk,
                                               const PurityModels& models) {
  svp::audio::WhisperInferenceResult result =
      svp::audio::run_whisper_inference(
          chunk.wav, models.whisper_dir, models.vad_model, chunk.chunk_id, 0,
          chunk.duration_us,
          svp::audio::whisper_runtime_threads(audio_test_thread_plan()),
          models.aligner_dir);
  if (!result.ran) {
    throw std::runtime_error("real ASR purity chunk did not run: " +
                             chunk.chunk_id);
  }
  return result;
}

// Drops every process-wide ASR cache so the next chunk runs exactly as it
// would as the first chunk of a fresh process.
svp::audio::WhisperInferenceResult infer_chunk_cold(
    const PurityChunk& chunk, const PurityModels& models) {
  svp::audio::release_whisper_cpp_model();
  svp::audio::release_phoneme_aligner();
  return infer_chunk(chunk, models);
}

void require_identical_words(const std::vector<svp::audio::AsrWord>& expected,
                             const std::vector<svp::audio::AsrWord>& actual,
                             const std::string& label) {
  if (expected.size() != actual.size()) {
    throw std::runtime_error(label + ": word count " +
                             std::to_string(actual.size()) + " != " +
                             std::to_string(expected.size()));
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const svp::audio::AsrWord& e = expected[i];
    const svp::audio::AsrWord& a = actual[i];
    // Exact equality, including confidence: a chunk must be a pure function
    // of its own samples, not merely close to one.
    if (e.text != a.text || e.start_us != a.start_us || e.end_us != a.end_us ||
        e.confidence != a.confidence || e.timing_source != a.timing_source) {
      throw std::runtime_error(label + ": word " + std::to_string(i) + " '" +
                               a.text + "' differs from its cold-run result");
    }
  }
}

void require_identical_results(const svp::audio::WhisperInferenceResult& expected,
                               const svp::audio::WhisperInferenceResult& actual,
                               const std::string& label) {
  require_identical_words(expected.all_words, actual.all_words, label);
  if (expected.alignment_status != actual.alignment_status) {
    throw std::runtime_error(label + ": alignment status differs");
  }
  if (expected.segments.size() != actual.segments.size()) {
    throw std::runtime_error(label + ": segment count differs");
  }
  for (std::size_t i = 0; i < expected.segments.size(); ++i) {
    const svp::audio::WhisperSegment& e = expected.segments[i];
    const svp::audio::WhisperSegment& a = actual.segments[i];
    if (e.text != a.text || e.start_us != a.start_us || e.end_us != a.end_us) {
      throw std::runtime_error(label + ": segment " + std::to_string(i) +
                               " differs from its cold-run result");
    }
  }
}

}  // namespace

// Distributed ASR assigns chunk ranges to arbitrary processes, so every
// chunk's result must be a pure function of its own samples. Each chunk of a
// real speech fixture is decoded cold (fresh process-equivalent caches) and
// then again on warm caches: right after a time-reversed or noisy copy that
// drives the decoder into temperature fallback, and in one plan-order pass.
// Every result must be bit-identical, confidence included.
void test_real_asr_chunk_results_are_independent_of_prior_chunks_when_enabled() {
  const char* model_cache_env = std::getenv("SVP_MODEL_CACHE_DIR");
  if (!model_cache_env || std::string(model_cache_env).empty()) {
    return;
  }
  if (!svp::audio::is_whisper_runtime_available()) {
    throw std::runtime_error(
        "SVP_MODEL_CACHE_DIR is set but whisper.cpp is unavailable");
  }

  const std::filesystem::path model_cache_root = model_cache_env;
  const std::optional<std::filesystem::path> vad_model =
      svp::audio::find_whisper_ggml_vad_model(
          model_cache_root / svp::models::kWhisperCppSileroVadModelId);
  if (!vad_model.has_value()) {
    throw std::runtime_error("whisper.cpp VAD model missing from " +
                             model_cache_root.string());
  }
  const PurityModels models{
      model_cache_root / svp::models::kWhisperSmallEnglishModelId, *vad_model,
      model_cache_root / svp::models::kWav2Vec2EspeakPhonemeModelId};

  const std::filesystem::path source_wav =
      std::filesystem::path(SVP_REPO_ROOT) /
      "fixtures/audio/sherpa-diarization/four-speaker.wav";
  const std::filesystem::path slice_dir =
      std::filesystem::temp_directory_path() / "svp-asr-chunk-purity-test";
  std::filesystem::remove_all(slice_dir);
  std::filesystem::create_directories(slice_dir);

  const std::vector<PurityChunk> chunks =
      slice_fixture_on_chunk_grid(source_wav, slice_dir);
  if (chunks.size() < 2) {
    throw std::runtime_error(
        "chunk purity needs a fixture spanning at least two ASR chunks");
  }

  // Each chunk starts from a cold process-equivalent state, then is decoded
  // again after each fallback-inducing chunk on the same warm caches.
  std::vector<svp::audio::WhisperInferenceResult> cold;
  cold.reserve(chunks.size());
  for (const PurityChunk& chunk : chunks) {
    cold.push_back(infer_chunk_cold(chunk, models));
    (void)infer_chunk(
        write_transformed_chunk(chunk, "_reversed", slice_dir, time_reversed),
        models);
    require_identical_results(cold.back(), infer_chunk(chunk, models),
                              "after time-reversed audio " + chunk.chunk_id);
    (void)infer_chunk(write_transformed_chunk(chunk, "_noisy", slice_dir,
                                              with_uniform_noise),
                      models);
    require_identical_results(cold.back(), infer_chunk(chunk, models),
                              "after noisy audio " + chunk.chunk_id);
  }
  const bool any_words = std::any_of(
      cold.begin(), cold.end(),
      [](const auto& result) { return !result.all_words.empty(); });
  if (!any_words) {
    throw std::runtime_error("chunk purity fixture decoded no words");
  }

  // The production order: one warm pass over the chunk plan.
  svp::audio::release_whisper_cpp_model();
  svp::audio::release_phoneme_aligner();
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    require_identical_results(cold[i], infer_chunk(chunks[i], models),
                              "plan-order pass " + chunks[i].chunk_id);
  }

  svp::audio::release_whisper_cpp_model();
  svp::audio::release_phoneme_aligner();
  std::filesystem::remove_all(slice_dir);
}
