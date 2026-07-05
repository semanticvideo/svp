#include "types.hpp"

#include <algorithm>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

std::vector<float> compute_embedding_for_sample_range(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& samples,
    std::int64_t start_us,
    std::int64_t end_us) {
  if (end_us <= start_us) return {};
  std::int64_t start_sample = start_us * kDiarizationSampleRate / 1000000;
  std::int64_t end_sample = end_us * kDiarizationSampleRate / 1000000;
  start_sample = std::max<std::int64_t>(0, start_sample);
  end_sample = std::min<std::int64_t>(
      static_cast<std::int64_t>(samples.size()), end_sample);
  const std::int64_t num_samples = end_sample - start_sample;
  if (num_samples < kMinSpeakerEmbeddingSamples) return {};

  const SherpaOnnxOnlineStream* stream = api.emb_create_stream(extractor);
  if (!stream) return {};
  api.stream_accept(stream,
                    kDiarizationSampleRate,
                    samples.data() + start_sample,
                    static_cast<int32_t>(num_samples));
  api.stream_input_finished(stream);

  std::vector<float> embedding;
  if (api.emb_is_ready(extractor, stream)) {
    const float* result = api.emb_compute(extractor, stream);
    if (result) {
      embedding.assign(result, result + embedding_dim);
      api.emb_destroy_vec(result);
    }
  }
  api.stream_destroy(stream);
  if (!embedding.empty()) normalize_embedding(embedding);
  return embedding;
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
