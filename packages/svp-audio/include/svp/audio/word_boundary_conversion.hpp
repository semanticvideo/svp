#pragma once

#include "svp/audio/ctc_forced_aligner.hpp"

#include <vector>

namespace svp::audio {

// One acoustically aligned word interval before convention conversion.
struct AlignedWordSpan {
  double start_seconds = 0.0;
  double end_seconds = 0.0;
};

// Converts acoustic word intervals into NLE-convention word starts.
//
// NLE word intervals are packed nearly contiguously: each boundary sits
// inside the acoustic transition between adjacent words rather than at the
// strict acoustic onset. The rules below were validated against a
// Premiere Pro reference on both Kaldi and wav2vec2 alignments:
//
//   gap >= 600ms      -> end of the final quiet run before the word
//                        (preserves real silences)
//   50ms <= gap       -> midpoint of the inter-word gap
//   gap < 50ms        -> phone-aware rules using the first phone of this
//                        word (f) and the last phone of the previous (l):
//     f is a plosive/nasal longer than 80ms:
//         l is a vowel shorter than 70ms -> midpoint(l.start, word.start)
//         otherwise                      -> midpoint(f.start, f.end)
//     f is a vowel longer than 200ms     -> midpoint(f.start, f.end)
//     l is a vowel different from f      -> midpoint(l.start, word.start)
//     otherwise                          -> midpoint(prev.end, word.start)
//
// phone_spans and tokens must come from the same CtcForcedAligner::align
// call; tokens[i].word_index groups spans into words. samples must be the
// same 16 kHz mono audio that was aligned. Returns one start per word.
[[nodiscard]] std::vector<double> convert_aligned_word_starts(
    const std::vector<AlignedWordSpan>& words,
    const std::vector<CtcPhoneSpan>& phone_spans,
    const std::vector<CtcAlignmentToken>& tokens,
    const std::vector<float>& samples);

}  // namespace svp::audio
