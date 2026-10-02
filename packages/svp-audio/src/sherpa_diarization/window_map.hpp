#pragma once

// The window map shared by run_sherpa_diarization and
// run_diarization_window_map (svp/audio/diarization_window_map.hpp).

#include "private.hpp"

#include "svp/audio/diarization_window_map.hpp"

#include <functional>
#include <string>

namespace svp::audio::sherpa_diarization_internal {

// The two model files of a diarization model bundle directory.
struct DiarizationModelFiles {
  std::string segmentation;
  std::string embedding;
};
[[nodiscard]] DiarizationModelFiles diarization_model_files(const std::filesystem::path& model_dir);

// run_sherpa_diarization's pipeline configuration over `files` (which must
// outlive the returned value: it points into their strings).
[[nodiscard]] SherpaOnnxOfflineSpeakerDiarizationConfig diarization_pipeline_config(
    const DiarizationModelFiles& files, const svp::models::SherpaThreadCounts& threads);

// The embedding extractor configuration over `files` (same lifetime rule).
[[nodiscard]] SherpaOnnxSpeakerEmbeddingExtractorConfig diarization_extractor_config(
    const DiarizationModelFiles& files, const svp::models::SherpaThreadCounts& threads);

// Whether the loaded library exposes every embedding extractor entry point.
[[nodiscard]] bool embedding_api_available(const SherpaDiarizationApi& api);

// Maps window `window_index` (`window`) of the WAV: a fresh pipeline from
// `config`, the window's samples, its feed pieces, clipping, window-local
// observations, and (when `extractor` is set) their embeddings. `on_piece`
// runs after each piece.
[[nodiscard]] DiarizationWindowOutcome map_diarization_window(
    const SherpaDiarizationApi& api, const SherpaOnnxOfflineSpeakerDiarizationConfig& config,
    const void* extractor, int32_t embedding_dim, const PcmS16MonoWavInfo& wav_info,
    const DiarizationWindowRange& window, std::size_t window_index,
    const std::function<void()>& on_piece);

}  // namespace svp::audio::sherpa_diarization_internal
