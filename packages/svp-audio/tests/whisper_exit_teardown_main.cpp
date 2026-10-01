// Regression test for process exit with a whisper.cpp model still cached.
//
// The whisper.cpp backend caches its model context for the life of the
// process unless release_whisper_cpp_model() is called. With the Metal
// backend that context holds Metal buffers, and ggml's Metal device teardown
// at exit asserts that every buffer was freed first. This program decodes one
// ASR chunk and then returns from main WITHOUT releasing the model, the way
// any caller on an error path or a test binary ends. It passes only if the
// process exits normally (status 0); the static-destruction-order bug aborted
// it with SIGABRT in ggml_metal_rsets_free.
//
// Runs only when SVP_MODEL_CACHE_DIR names a model cache; otherwise it reports
// a skip and exits 0, like the other real-model audio tests.

#include "audio_test_support.hpp"

#include "svp/audio/asr_chunk_context.hpp"
#include "svp/audio/wav_slice.hpp"
#include "svp/audio/whisper_cpp_model.hpp"
#include "svp/audio/whisper_pcm_reader.hpp"
#include "svp/models/reference_processor_model_ids.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

int run() {
  const char* model_cache_env = std::getenv("SVP_MODEL_CACHE_DIR");
  if (model_cache_env == nullptr || std::string(model_cache_env).empty()) {
    std::cout << "svp-audio-exit-teardown-test: skipped (SVP_MODEL_CACHE_DIR unset)\n";
    return 0;
  }
  if (!svp::audio::is_whisper_runtime_available()) {
    throw std::runtime_error("SVP_MODEL_CACHE_DIR is set but whisper.cpp is unavailable");
  }

  const std::filesystem::path model_cache_root = model_cache_env;
  const std::optional<std::filesystem::path> vad_model =
      svp::audio::find_whisper_ggml_vad_model(
          model_cache_root / svp::models::kWhisperCppSileroVadModelId);
  if (!vad_model.has_value()) {
    throw std::runtime_error("whisper.cpp VAD model missing from " +
                             model_cache_root.string());
  }

  // Any speech fixture works; decode the first chunk of the production chunk
  // plan so the model is loaded and used exactly as a build uses it.
  const std::filesystem::path source_wav =
      std::filesystem::path(SVP_REPO_ROOT) /
      "fixtures/audio/sherpa-diarization/one-speaker.wav";
  const std::filesystem::path slice_dir =
      std::filesystem::temp_directory_path() / "svp-audio-exit-teardown-test";
  std::filesystem::remove_all(slice_dir);
  std::filesystem::create_directories(slice_dir);

  const std::size_t sample_count =
      svp::audio::read_whisper_pcm16_mono_wav(source_wav).size();
  constexpr std::int64_t kMicrosecondsPerSecond = 1000000;
  // read_whisper_pcm16_mono_wav requires the 16 kHz analysis format.
  constexpr std::int64_t kAnalysisSampleRate = 16000;
  const svp::audio::AsrChunkPlanResult plan = svp::audio::build_asr_chunk_plan(
      static_cast<std::int64_t>(sample_count) * kMicrosecondsPerSecond /
      kAnalysisSampleRate);
  if (plan.chunks.empty()) {
    throw std::runtime_error("exit teardown fixture produced no ASR chunk");
  }
  const svp::audio::AsrChunkContextPlan context =
      svp::audio::plan_asr_chunk_context(plan.chunks.front());
  const std::filesystem::path chunk_wav = svp::audio::slice_wav_to_temp(
      source_wav, context.slice_start_us, context.slice_end_us, slice_dir);

  const svp::audio::WhisperInferenceResult result = svp::audio::run_whisper_inference(
      chunk_wav, model_cache_root / svp::models::kWhisperSmallEnglishModelId,
      *vad_model, plan.chunks.front().chunk_id, 0,
      context.slice_end_us - context.slice_start_us,
      svp::audio::whisper_runtime_threads(audio_test_thread_plan()));
  std::filesystem::remove_all(slice_dir);
  if (!result.ran || result.all_words.empty()) {
    throw std::runtime_error("exit teardown chunk did not decode speech");
  }

  // Deliberately no release_whisper_cpp_model(): the cached context must be
  // torn down safely by normal process exit.
  std::cout << "svp-audio-exit-teardown-test: decoded " << result.all_words.size()
            << " words; exiting with the model cached\n";
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& error) {
    std::cerr << "svp-audio-exit-teardown-test: " << error.what() << "\n";
    return 1;
  }
}
