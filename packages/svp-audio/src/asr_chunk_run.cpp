#include "svp/audio/asr_chunk_run.hpp"

#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/wav_slice.hpp"
#include "svp/audio/whisper_cpp_model.hpp"
#include "svp/audio/whisper_model.hpp"

#include <system_error>

namespace svp::audio {

AsrChunkOutcome run_asr_chunk(const std::filesystem::path& input_wav, const AsrChunkPlan& chunk,
                              std::int64_t chunk_ordinal, const AsrChunkModels& models,
                              const WhisperRuntimeThreads& threads,
                              const std::filesystem::path& slice_dir) {
  const AsrChunkContextPlan context = plan_asr_chunk_context(chunk);
  const std::optional<std::filesystem::path> slice = slice_wav_range_to_temp(
      input_wav, context.slice_start_us, context.slice_end_us, slice_dir);
  if (!slice) {
    // The chunk plan follows the container's duration, which can run past
    // the end of the audio (a video longer than its audio track). A chunk
    // with no audio samples is silence: it decodes to no words, as a chunk
    // whose VAD finds no speech does, rather than blocking the transcript.
    AsrChunkOutcome silent;
    silent.ran = true;
    silent.alignment_status = WhisperInferenceResult{}.alignment_status;
    return silent;
  }
  const std::filesystem::path& chunk_wav = *slice;

  WhisperInferenceResult whisper_result;
  try {
    whisper_result = run_whisper_inference(chunk_wav, models.model_dir, models.vad_model_path,
                                           chunk.chunk_id, 0,
                                           context.slice_end_us - context.slice_start_us,
                                           threads, models.aligner_model_dir);
  } catch (...) {
    std::error_code cleanup_error;
    std::filesystem::remove(chunk_wav, cleanup_error);
    throw;
  }
  std::error_code cleanup_error;
  std::filesystem::remove(chunk_wav, cleanup_error);

  AsrChunkOutcome outcome;
  outcome.ran = whisper_result.ran;
  outcome.decoded_word_count = whisper_result.all_words.size();
  outcome.alignment_status = whisper_result.alignment_status;
  outcome.alignment_error = whisper_result.alignment_error;
  if (!whisper_result.ran) {
    outcome.blockers = whisper_result.blockers;
    return outcome;
  }
  outcome.words =
      retain_nominal_chunk_words(whisper_result.all_words, context, chunk, chunk_ordinal);
  return outcome;
}

std::optional<AsrChunkModels> resolve_asr_chunk_models(
    const std::filesystem::path& model_cache_root, const std::string& model_id,
    const std::string& vad_model_id, const std::optional<std::string>& alignment_model_id,
    std::string& why) {
  try {
    if (!verify_asr_model_files(model_id, model_cache_root)) {
      why = "Whisper ASR model " + model_id + " is missing or does not verify";
      return std::nullopt;
    }
    const std::optional<std::filesystem::path> vad =
        find_whisper_ggml_vad_model(model_cache_root / vad_model_id);
    if (!vad || !verify_asr_model_files(vad_model_id, model_cache_root)) {
      why = "Silero VAD model " + vad_model_id + " is missing or does not verify";
      return std::nullopt;
    }
    AsrChunkModels models{.model_dir = model_cache_root / model_id,
                          .vad_model_path = *vad,
                          .aligner_model_dir = {}};
    if (alignment_model_id) {
      if (!verify_asr_model_files(*alignment_model_id, model_cache_root)) {
        why = "phoneme aligner model " + *alignment_model_id + " is missing or does not verify";
        return std::nullopt;
      }
      models.aligner_model_dir = model_cache_root / *alignment_model_id;
    }
    return models;
  } catch (const std::exception& error) {
    why = std::string("ASR models could not be resolved: ") + error.what();
    return std::nullopt;
  }
}

}  // namespace svp::audio
