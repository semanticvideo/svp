#include "private.hpp"

#include <cstring>
#include <filesystem>

namespace svp::audio {

std::vector<std::string> replay_word_speaker_assignments(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::vector<AsrWord>& words,
    const SherpaDiarizationResult& diar_result) {
  if (words.empty() || diar_result.final_speaker_count <= 1 ||
      diar_result.segments.empty()) {
    return {};
  }

  const sherpa_diarization_internal::SherpaDiarizationApi& api =
      sherpa_diarization_internal::get_api();
  if (!api.lib_handle || !api.emb_create) {
    return {};
  }

  const std::filesystem::path embedding_model =
      model_dir / "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";
  if (!std::filesystem::exists(embedding_model)) {
    return {};
  }

  sherpa_diarization_internal::PcmS16MonoWavInfo wav_info;
  try {
    wav_info = sherpa_diarization_internal::read_pcm_s16le_mono_wav_info(wav_path);
  } catch (...) {
    return {};
  }

  const std::string emb_path = embedding_model.string();
  sherpa_diarization_internal::SherpaOnnxSpeakerEmbeddingExtractorConfig
      emb_config;
  std::memset(&emb_config, 0, sizeof(emb_config));
  emb_config.model = emb_path.c_str();
  emb_config.num_threads = 1;
  emb_config.debug = 0;
  emb_config.provider = "cpu";

  const void* extractor = api.emb_create(&emb_config);
  if (!extractor) return {};

  const int32_t embedding_dim = api.emb_dim(extractor);
  std::vector<std::string> assignments =
      sherpa_diarization_internal::assign_word_speakers_with_extractor(
          api, extractor, embedding_dim, wav_info, words, diar_result);
  api.emb_destroy(extractor);
  return assignments;
}

}  // namespace svp::audio
