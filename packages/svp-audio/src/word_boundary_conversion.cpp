#include "svp/audio/word_boundary_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>

namespace svp::audio {
namespace {

// Inter-word gap this large is a real pause; the boundary anchors at the
// acoustic onset edge instead of the gap midpoint.
constexpr double kSilenceGapSeconds = 0.6;
// Gaps at or above this size are split at their midpoint.
constexpr double kGapMidMinSeconds = 0.05;
// Word-initial closures longer than this are split at their midpoint.
constexpr double kClosureMinSeconds = 0.08;
// A previous vowel shorter than this cannot carry the boundary; the split
// moves to the vowel-start/word-start midpoint.
constexpr double kShortVowelSeconds = 0.07;
// A first vowel longer than this is split at its own midpoint.
constexpr double kLongVowelSeconds = 0.2;
// Quiet-run search window around the aligned onset for long gaps.
constexpr double kOnsetSearchBackSeconds = 0.7;
constexpr double kOnsetSearchForwardSeconds = 0.05;
// Onset edge: rising crossing of gap noise floor + this margin.
constexpr double kOnsetFloorMarginDb = 6.0;

// Energy analysis grid: 20ms RMS windows on a 2ms hop over 16 kHz audio.
constexpr double kSampleRate = 16000.0;
constexpr int kEnergyHopSamples = 32;
constexpr int kEnergyWindowSamples = 320;

bool is_closure(const std::string& phone) {
  // espeak plosives and affricates (word-initial closure class), including
  // aspirated, breathy, palatalized, and geminate English realizations.
  static const std::unordered_set<std::string> phones = {
      "p",  "pʰ", "ph", "pʲ", "pː", "b",  "bʰ", "bʲ", "bː",
      "t",  "tʰ", "th", "tʲ", "t̪", "t[", "t^", "tː", "t^ː", "t̪ː",
      "d",  "dʰ", "dʲ", "dˤ", "d[", "d^", "dː", "dʲʲ", "dˤdˤ", "d̪",
      "k",  "kʰ", "kh", "kʲ", "kː", "ɡ", "ɡʰ", "ɡʲ", "ɡː", "g",
      "c",  "cʰ", "cː", "cʰcʰ", "ɟ", "ɟʰ", "ɟː",
      "ʈ",  "ʈʰ", "ɖ", "ɖʰ", "q",  "qː", "ʔ",
      "tʃ", "tʃʰ", "tʃʲ", "tʃː", "tS", "dʒ", "dʒʲ", "dʒː", "dZ",
      "ts", "tsʲ", "tsː", "ts.", "ts.h", "tsh", "tɕ", "tɕh", "tɕʲ",
      "dʑ", "dʑʲ", "dzː", "pf", "sx",
  };
  return phones.count(phone) != 0;
}

bool is_nasal(const std::string& phone) {
  static const std::unordered_set<std::string> phones = {
      "m", "mʲ", "n", "nʲ", "nʲʲ", "ɲ", "ɳ", "ŋ", "ɴ",
  };
  return phones.count(phone) != 0;
}

bool is_vowel(const std::string& phone) {
  // espeak monophthongs, diphthongs, r-colored nuclei, and syllabic
  // consonants that carry the syllable nucleus for English.
  static const std::unordered_set<std::string> phones = {
      "a",  "aː", "a.", "a.ː", "ä",  "æ",  "æː", "ɐ",  "ɐɐ", "ɑ",
      "ɑː", "ɑ:", "ɒ",  "ɔ",  "ɔː",  "e",  "eː", "e:", "ee", "ɛ",
      "ɛː", "ə",  "ɜ",  "ɜː", "i",   "iː", "i:", "ɪ",  "ɨ",  "ɨː",
      "ɯ",  "o",  "oː", "o:", "ɵ",   "ɵː", "ø",  "øː", "œ",  "œː",
      "u",  "uː", "u:", "ʉ",  "ʊ",   "ʌ",  "y",  "yː", "y:", "ᵻ",
      "aɪ", "ai", "eɪ", "ɔɪ", "oɪ",  "aʊ", "au", "əʊ", "oʊ", "eʊ",
      "ɛʊ", "ɛɪ", "ɪʊ", "iʊ", "æi",  "æiː", "ei", "ou",
      "ɪə", "iə", "eə", "ʊə", "iɛ",  "aɪə", "aɪɚ",
      "ɪɹ", "ɛɹ", "ʊɹ", "ɑːɹ", "ɔːɹ", "oːɹ", "aɜ", "eɑ", "uɜ",
      "l̩",  "n̩",  "m̩",  "r̩",  "ɚ",
  };
  return phones.count(phone) != 0;
}

// RMS energy in dB on the analysis grid; tm[i] is each window's center.
struct EnergyGrid {
  std::vector<double> db;
  std::vector<double> tm;
};

EnergyGrid energy_grid(const std::vector<float>& samples) {
  EnergyGrid grid;
  if (samples.size() < static_cast<std::size_t>(kEnergyWindowSamples)) {
    return grid;
  }
  const std::size_t frames =
      (samples.size() - kEnergyWindowSamples) / kEnergyHopSamples;
  grid.db.reserve(frames);
  grid.tm.reserve(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const float* window = samples.data() + i * kEnergyHopSamples;
    double sum_sq = 0.0;
    for (int s = 0; s < kEnergyWindowSamples; ++s) {
      sum_sq += static_cast<double>(window[s]) * window[s];
    }
    const double rms = std::sqrt(sum_sq / kEnergyWindowSamples);
    grid.db.push_back(20.0 * std::log10(rms + 1e-9));
    grid.tm.push_back(
        (i * kEnergyHopSamples + kEnergyWindowSamples / 2.0) /
        kSampleRate);
  }
  return grid;
}

double percentile_10(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double pos = 0.10 * static_cast<double>(values.size() - 1);
  const std::size_t low = static_cast<std::size_t>(pos);
  const std::size_t high =
      std::min(low + 1, values.size() - 1);
  const double frac = pos - static_cast<double>(low);
  return values[low] + frac * (values[high] - values[low]);
}

// End of the final quiet run before the aligned onset: the last point
// where energy rises through the gap's noise floor + margin.
double onset_edge(const EnergyGrid& grid, double prev_end,
                  double aligned_start) {
  if (grid.tm.empty()) return aligned_start;
  const auto at_or_after = [&](double t) {
    return static_cast<std::size_t>(
        std::lower_bound(grid.tm.begin(), grid.tm.end(), t) -
        grid.tm.begin());
  };
  const std::size_t i0 = at_or_after(prev_end);
  const std::size_t i1 = at_or_after(aligned_start);
  if (i1 <= i0 || i1 > grid.db.size()) return aligned_start;
  const double floor =
      percentile_10({grid.db.begin() + static_cast<std::ptrdiff_t>(i0),
                     grid.db.begin() + static_cast<std::ptrdiff_t>(i1)}) +
      kOnsetFloorMarginDb;
  const std::size_t j0 = at_or_after(aligned_start - kOnsetSearchBackSeconds);
  const std::size_t j1 = std::min(
      at_or_after(aligned_start + kOnsetSearchForwardSeconds),
      grid.db.size() - 1);
  std::size_t last = grid.db.size();
  for (std::size_t k = j0; k < j1; ++k) {
    if (grid.db[k] < floor && grid.db[k + 1] >= floor) last = k + 1;
  }
  return last < grid.tm.size() ? grid.tm[last] : aligned_start;
}

}  // namespace

std::vector<double> convert_aligned_word_starts(
    const std::vector<AlignedWordSpan>& words,
    const std::vector<CtcPhoneSpan>& phone_spans,
    const std::vector<CtcAlignmentToken>& tokens,
    const std::vector<float>& samples) {
  std::vector<double> starts;
  starts.reserve(words.size());
  if (words.empty()) return starts;
  starts.push_back(words[0].start_seconds);

  const EnergyGrid grid = energy_grid(samples);
  for (std::size_t i = 1; i < words.size(); ++i) {
    const double cur_start = words[i].start_seconds;
    const double prev_end = words[i - 1].end_seconds;
    const double gap = cur_start - prev_end;
    if (gap >= kSilenceGapSeconds) {
      starts.push_back(onset_edge(grid, prev_end, cur_start));
      continue;
    }
    if (gap >= kGapMidMinSeconds) {
      starts.push_back((prev_end + cur_start) / 2.0);
      continue;
    }

    const CtcPhoneSpan* first = nullptr;
    const CtcPhoneSpan* last = nullptr;
    std::string first_phone;
    std::string last_phone;
    for (std::size_t s = 0; s < phone_spans.size(); ++s) {
      const std::size_t owner = tokens[phone_spans[s].token_index].word_index;
      if (owner == i && first == nullptr) {
        first = &phone_spans[s];
        first_phone = tokens[phone_spans[s].token_index].phone;
      }
      if (owner == i - 1) {
        last = &phone_spans[s];
        last_phone = tokens[phone_spans[s].token_index].phone;
      }
    }
    const double first_dur =
        first ? first->end_seconds - first->start_seconds : 0.0;
    const double last_dur =
        last ? last->end_seconds - last->start_seconds : 0.0;

    if (first != nullptr &&
        (is_closure(first_phone) || is_nasal(first_phone)) &&
        first_dur > kClosureMinSeconds) {
      if (last != nullptr && is_vowel(last_phone) &&
          last_dur < kShortVowelSeconds) {
        starts.push_back((last->start_seconds + cur_start) / 2.0);
      } else {
        starts.push_back(
            (first->start_seconds + first->end_seconds) / 2.0);
      }
      continue;
    }
    if (first != nullptr && is_vowel(first_phone) &&
        first_dur > kLongVowelSeconds) {
      starts.push_back(
          (first->start_seconds + first->end_seconds) / 2.0);
      continue;
    }
    if (last != nullptr && is_vowel(last_phone) &&
        first_phone != last_phone) {
      starts.push_back((last->start_seconds + cur_start) / 2.0);
      continue;
    }
    starts.push_back((prev_end + cur_start) / 2.0);
  }
  return starts;
}

}  // namespace svp::audio
