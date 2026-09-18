#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace svp::audio {

// One phone emitted by the CTC token sequence. word_index ties the token
// back to the orthographic word it was generated from.
struct CtcAlignmentToken {
  std::string phone;
  std::size_t word_index = 0;
};

// A contiguous span of aligned frames belonging to one input token.
struct CtcPhoneSpan {
  std::size_t token_index = 0;
  double start_seconds = 0.0;
  double end_seconds = 0.0;
};

// Deterministic CTC forced aligner over the wav2vec2 espeak phoneme ONNX
// model. Runs one non-autoregressive forward pass and a Viterbi trellis
// over the emitted phone sequence; no sampling or randomness is involved.
class CtcForcedAligner {
 public:
  // bundle_dir must contain model.svpmodel.json plus the ONNX weights,
  // vocabulary, and pronunciation dictionary files declared by the bundle
  // manifest. Throws on missing or invalid artifacts.
  [[nodiscard]] static CtcForcedAligner load(
      const std::filesystem::path& bundle_dir);

  // Aligns 16 kHz mono float samples to the given phone token sequence.
  // Returns one span per token in input order. Throws on runtime failure.
  [[nodiscard]] std::vector<CtcPhoneSpan> align(
      const std::vector<float>& samples,
      const std::vector<CtcAlignmentToken>& tokens) const;

 private:
  struct Impl;
  explicit CtcForcedAligner(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

}  // namespace svp::audio
