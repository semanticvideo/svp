#include "svp/audio/whisper_model.hpp"

#include "svp/audio/whisper_cpp_backend.hpp"
#include "svp/audio/whisper_cpp_model.hpp"

#include <exception>

namespace svp::audio {

void set_whisper_verbose(bool verbose) {
  set_whisper_cpp_verbose(verbose);
}

bool is_whisper_runtime_available() {
  return is_whisper_cpp_runtime_available();
}

WhisperInferenceResult run_whisper_inference(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::string& chunk_id,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us) {
  try {
    const auto ggml_model = find_whisper_ggml_model(model_dir);
    if (!ggml_model.has_value()) {
      WhisperInferenceResult result;
      result.blockers.push_back(
          "The model bundle does not contain exactly one GGML Whisper model");
      return result;
    }
    (void)chunk_id;
    return run_whisper_cpp_inference(wav_path, *ggml_model, chunk_start_us,
                                     chunk_end_us);
  } catch (const std::exception& e) {
    WhisperInferenceResult result;
    result.blockers.push_back(std::string("Whisper inference failed: ") +
                              e.what());
    return result;
  }
}

}  // namespace svp::audio
