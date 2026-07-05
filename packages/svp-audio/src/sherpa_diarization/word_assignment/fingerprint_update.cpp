#include "types.hpp"

#include <algorithm>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

bool update_fingerprint(VoiceFingerprint& fingerprint,
                        const std::vector<float>& embedding) {
  if (!has_embedding_signal(embedding)) return false;
  if (!has_embedding_signal(fingerprint.prototype)) {
    fingerprint.prototype = embedding;
    fingerprint.embedding_count = 1;
    return true;
  }
  if (fingerprint.prototype.size() != embedding.size()) return false;

  const std::size_t capped_count =
      std::min(fingerprint.embedding_count, kFingerprintUpdateMaxEmbeddings);
  const float prior_weight = static_cast<float>(capped_count);
  for (std::size_t i = 0; i < fingerprint.prototype.size(); ++i) {
    fingerprint.prototype[i] =
        (fingerprint.prototype[i] * prior_weight + embedding[i]) /
        (prior_weight + 1.0f);
  }
  normalize_embedding(fingerprint.prototype);
  ++fingerprint.embedding_count;
  return true;
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
