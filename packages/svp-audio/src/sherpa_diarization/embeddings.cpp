#include "private.hpp"

#include <algorithm>
#include <cmath>

namespace svp::audio::sherpa_diarization_internal {

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
  float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  if (norm_a < 1e-12f || norm_b < 1e-12f) return 0.0f;
  return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

void normalize_embedding(std::vector<float>& v) {
  float norm = 0.0f;
  for (float x : v) norm += x * x;
  norm = std::sqrt(norm);
  if (norm > 1e-12f) {
    for (float& x : v) x /= norm;
  }
}

std::vector<float> compute_bounded_speaker_embedding(
    const SherpaDiarizationApi& api,
    const void* extractor,
    int32_t embedding_dim,
    const std::vector<float>& all_samples,
    std::size_t sample_base,
    const std::vector<SherpaDiarizationSegment>& speaker_segments) {

  std::vector<float> concatenated;
  concatenated.reserve(static_cast<std::size_t>(kMaxSpeakerEmbeddingSamples));

  for (const auto& seg : speaker_segments) {
    std::int64_t absolute_start =
        static_cast<std::int64_t>(seg.start_sec * 16000.0f);
    std::int64_t absolute_end =
        static_cast<std::int64_t>(seg.end_sec * 16000.0f);
    if (absolute_start < static_cast<std::int64_t>(sample_base)) {
      absolute_start = static_cast<std::int64_t>(sample_base);
    }
    const std::int64_t sample_limit =
        static_cast<std::int64_t>(sample_base + all_samples.size());
    if (absolute_end > sample_limit) absolute_end = sample_limit;
    const std::int64_t relative_start =
        absolute_start - static_cast<std::int64_t>(sample_base);
    const std::int64_t relative_end =
        absolute_end - static_cast<std::int64_t>(sample_base);
    int32_t start_sample = static_cast<int32_t>(relative_start);
    int32_t end_sample = static_cast<int32_t>(relative_end);
    int32_t num_samples = end_sample - start_sample;
    if (num_samples < 1600) continue;

    if (static_cast<int32_t>(concatenated.size()) + num_samples >
        kMaxSpeakerEmbeddingSamples) {
      int32_t room = kMaxSpeakerEmbeddingSamples -
                     static_cast<int32_t>(concatenated.size());
      if (room > 0) {
        concatenated.insert(concatenated.end(),
                            all_samples.data() + start_sample,
                            all_samples.data() + start_sample + room);
      }
      break;
    }
    concatenated.insert(concatenated.end(),
                        all_samples.data() + start_sample,
                        all_samples.data() + end_sample);
  }

  if (concatenated.size() < static_cast<std::size_t>(kMinSpeakerEmbeddingSamples)) {
    return std::vector<float>(embedding_dim, 0.0f);
  }

  const SherpaOnnxOnlineStream* stream = api.emb_create_stream(extractor);
  if (!stream) {
    return std::vector<float>(embedding_dim, 0.0f);
  }

  api.stream_accept(stream, 16000, concatenated.data(),
                    static_cast<int32_t>(concatenated.size()));
  api.stream_input_finished(stream);

  std::vector<float> embedding;
  if (api.emb_is_ready(extractor, stream)) {
    const float* emb = api.emb_compute(extractor, stream);
    if (emb) {
      embedding.assign(emb, emb + embedding_dim);
      api.emb_destroy_vec(emb);
    }
  }
  api.stream_destroy(stream);

  if (embedding.empty()) {
    return std::vector<float>(embedding_dim, 0.0f);
  }
  normalize_embedding(embedding);
  return embedding;
}


bool has_embedding_signal(const std::vector<float>& embedding) {
  for (float value : embedding) {
    if (std::abs(value) > 1e-6f) return true;
  }
  return false;
}

}  // namespace svp::audio::sherpa_diarization_internal
