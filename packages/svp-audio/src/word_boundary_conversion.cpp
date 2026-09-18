#include "svp/audio/word_boundary_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
// Span head this far above the quiet floor counts as real speech.
constexpr double kSpanHeadVoicedMarginDb = 12.0;
// Head-level window for judging whether a span starts in speech.
constexpr double kSpanHeadProbeSeconds = 0.3;
// Lead-in ahead of a post-silence acoustic attack: an NLE boundary marks a
// resuming word before the audible onset so a transcript cut does not clip
// the approach, but never earlier than the middle of the quiet dip.
constexpr double kOnsetLeadBackSeconds = 0.3;
// Loud excursions shorter than this inside a pause are breaths or clicks;
// they do not end the quiet run when locating a dip's start.
constexpr double kQuietBlipMaxSeconds = 0.12;
// An NLE-style packed boundary rides the first stretch of the valley
// between words — it belongs to the previous word's decay, not the next
// word's rise.
constexpr double kValleyBoundaryFraction = 0.35;
// The resumption scan stops one onset lead-in short of the next word's
// span start: a rise that close to the next span is that word's acoustic
// approach, never this word's resumption.
constexpr double kResumeScanMarginSeconds = kOnsetLeadBackSeconds;

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

double median(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// A quiet-to-loud transition: edge is the rising point (the word's acoustic
// onset side), start is where the quiet run that edge closes began (the
// previous speech's offset side). found is false when no quiet run exists.
struct DipEdge {
  double edge = 0.0;
  double start = 0.0;
  bool found = false;
};

// Word onset boundary: an NLE marks the seam inside the first stretch of
// the valley (the previous word's decay side), while a post-pause onset
// also gets a fixed lead-in ahead of the acoustic attack so a transcript
// cut does not clip the approach. The later of the two wins: the valley
// fraction governs short dips, the lead-in governs real pauses.
double lead_back_onset(const DipEdge& dip, double fallback) {
  if (!dip.found) return fallback;
  return std::max(dip.start +
                      kValleyBoundaryFraction * (dip.edge - dip.start),
                  dip.edge - kOnsetLeadBackSeconds);
}

// First index of the quiet run that ends at `rise`: walking back over
// below-floor samples and skipping loud excursions shorter than
// kQuietBlipMaxSeconds, bounded below by `lo`.
std::size_t quiet_run_begin(const EnergyGrid& grid, std::size_t rise,
                            std::size_t lo, double floor) {
  const std::size_t blip_frames = static_cast<std::size_t>(
      kQuietBlipMaxSeconds / (kEnergyHopSamples / kSampleRate));
  std::size_t j = rise;
  while (j > lo) {
    if (grid.db[j - 1] < floor) {
      --j;
      continue;
    }
    const std::size_t loud_end = j - 1;
    std::size_t loud_begin = loud_end;
    while (loud_begin > lo && grid.db[loud_begin - 1] >= floor) {
      --loud_begin;
    }
    if (loud_end - loud_begin + 1 >= blip_frames) {
      return loud_end + 1;
    }
    j = loud_begin;
  }
  return lo;
}

// End of the final quiet run before the aligned onset: the last point
// where energy rises through the gap's noise floor + margin, plus where
// that quiet run began.
DipEdge onset_edge(const EnergyGrid& grid, double prev_end,
                   double aligned_start) {
  DipEdge dip;
  if (grid.tm.empty()) return dip;
  const auto at_or_after = [&](double t) {
    return static_cast<std::size_t>(
        std::lower_bound(grid.tm.begin(), grid.tm.end(), t) -
        grid.tm.begin());
  };
  const std::size_t i0 = at_or_after(prev_end);
  const std::size_t i1 = at_or_after(aligned_start);
  if (i1 <= i0 || i1 > grid.db.size()) return dip;
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
  if (last >= grid.tm.size()) return dip;
  dip.edge = grid.tm[last];
  dip.start = grid.tm[quiet_run_begin(grid, last, j0, floor)];
  dip.found = true;
  return dip;
}

// When the trellis smears a word's phones across a real pause, the aligned
// span starts inside the gap while the speech sits at its tail. A leading
// quiet stretch longer than a real pause cannot belong to the word, so the
// onset snaps to the last quiet-to-loud rise inside the span. Two guards:
// the scan is bounded by scan_limit (the decoder's own interval end, when
// known) because a span can also over-extend into the next word's onset —
// a rise past the decoder end belongs to that word, not this one; and a
// clearly voiced span head keeps its start unless the word's vowel nucleus
// sits at or past the resumption, because a consonant phone can smear onto
// a noise blip at the gap head while the nucleus stays with the real
// speech. nucleus_start is the word's first vowel phone onset, or negative
// when no phone evidence exists; without it a voiced head keeps its start.
DipEdge resumption_edge(const EnergyGrid& grid, double span_start,
                        double span_end, double nucleus_start,
                        double scan_limit) {
  DipEdge dip;
  if (grid.tm.empty() || span_end - span_start < kSilenceGapSeconds) {
    return dip;
  }
  const auto at_or_after = [&](double t) {
    return static_cast<std::size_t>(
        std::lower_bound(grid.tm.begin(), grid.tm.end(), t) -
        grid.tm.begin());
  };
  const std::size_t i0 = at_or_after(span_start);
  const std::size_t i1 = std::min(
      at_or_after(std::min(span_end, scan_limit)), grid.db.size() - 1);
  if (i1 <= i0) return dip;
  const double base =
      percentile_10({grid.db.begin() + static_cast<std::ptrdiff_t>(i0),
                     grid.db.begin() + static_cast<std::ptrdiff_t>(i1)});
  const double floor = base + kOnsetFloorMarginDb;
  const double voiced = base + kSpanHeadVoicedMarginDb;
  std::size_t last = grid.db.size();
  for (std::size_t k = i0; k < i1; ++k) {
    if (grid.db[k] < floor && grid.db[k + 1] >= floor) last = k + 1;
  }
  if (last >= grid.tm.size() ||
      grid.tm[last] - span_start < kSilenceGapSeconds) {
    return dip;
  }
  // The resumption must reach speech level past the floor crossing: a
  // noise blip inside a pause is not the word's onset.
  const std::size_t probe_end = std::min(
      last + static_cast<std::size_t>(kSpanHeadProbeSeconds /
                                      (kEnergyHopSamples / kSampleRate)),
      grid.db.size() - 1);
  double rise_peak = -300.0;
  for (std::size_t k = last; k <= probe_end; ++k) {
    rise_peak = std::max(rise_peak, grid.db[k]);
  }
  if (rise_peak < voiced) return dip;
  // Median level over the span's first moments; a few noise blips do not
  // count as a voiced head.
  const std::size_t head_end =
      std::min(i0 + static_cast<std::size_t>(
                        kSpanHeadProbeSeconds / (kEnergyHopSamples /
                                                 kSampleRate)),
               i1);
  const double head_median = median(
      {grid.db.begin() + static_cast<std::ptrdiff_t>(i0),
       grid.db.begin() + static_cast<std::ptrdiff_t>(head_end)});
  if (head_median >= voiced &&
      (nucleus_start < 0.0 || nucleus_start < grid.tm[last])) {
    return dip;
  }
  dip.edge = grid.tm[last];
  dip.start = grid.tm[quiet_run_begin(grid, last, i0, floor)];
  dip.found = true;
  return dip;
}

// Quiet dip inside an inter-word gap: the last below-floor run's edges,
// letting the boundary sit mid-dip or a lead-in ahead of the rise.
DipEdge gap_dip(const EnergyGrid& grid, double prev_end, double cur_start) {
  DipEdge dip;
  // Dips this narrow are a few grid frames at most; below that there is no
  // measurable valley and the phonetic rules decide the seam.
  constexpr double kDipMinGapSeconds = 0.02;
  if (grid.tm.empty() || cur_start - prev_end < kDipMinGapSeconds) {
    return dip;
  }
  const auto at_or_after = [&](double t) {
    return static_cast<std::size_t>(
        std::lower_bound(grid.tm.begin(), grid.tm.end(), t) -
        grid.tm.begin());
  };
  const std::size_t i0 = at_or_after(prev_end);
  const std::size_t i1 = std::min(
      at_or_after(cur_start + kOnsetSearchForwardSeconds),
      grid.db.size() - 1);
  if (i1 <= i0 + 1) return dip;
  const double floor =
      percentile_10({grid.db.begin() + static_cast<std::ptrdiff_t>(i0),
                     grid.db.begin() + static_cast<std::ptrdiff_t>(i1)}) +
      kOnsetFloorMarginDb;
  std::size_t last = grid.db.size();
  for (std::size_t k = i0; k < i1; ++k) {
    if (grid.db[k] < floor && grid.db[k + 1] >= floor) last = k + 1;
  }
  if (last >= grid.tm.size()) return dip;
  dip.edge = grid.tm[last];
  dip.start = grid.tm[quiet_run_begin(grid, last, i0, floor)];
  dip.found = true;
  return dip;
}

}  // namespace

std::vector<double> snap_word_starts_to_resumptions(
    const std::vector<AlignedWordSpan>& words,
    const std::vector<float>& samples) {
  const EnergyGrid grid = energy_grid(samples);
  std::vector<double> starts;
  starts.reserve(words.size());
  for (std::size_t i = 0; i < words.size(); ++i) {
    const AlignedWordSpan& word = words[i];
    double scan_limit = word.end_seconds;
    if (i + 1 < words.size()) {
      // A rise leading into the next word's span belongs to that word; the
      // onset edge leads the span start by closure and aspiration time.
      const DipEdge next_onset =
          gap_dip(grid, word.end_seconds, words[i + 1].start_seconds);
      const double claimed =
          (next_onset.found ? next_onset.edge : words[i + 1].start_seconds) -
          kResumeScanMarginSeconds;
      scan_limit = std::max(word.end_seconds, claimed);
    }
    starts.push_back(lead_back_onset(
        resumption_edge(grid, word.start_seconds, word.end_seconds, -1.0,
                        scan_limit),
        word.start_seconds));
  }
  return starts;
}

std::vector<double> convert_aligned_word_starts(
    const std::vector<AlignedWordSpan>& words,
    const std::vector<CtcPhoneSpan>& phone_spans,
    const std::vector<CtcAlignmentToken>& tokens,
    const std::vector<float>& samples,
    const std::vector<double>& decoder_end_seconds) {
  std::vector<double> starts;
  starts.reserve(words.size());
  if (words.empty()) return starts;
  const EnergyGrid grid = energy_grid(samples);
  // First vowel phone onset per word; negative when the word has no vowel
  // or no aligned phones at all.
  std::vector<double> vowel_start(words.size(), -1.0);
  for (const CtcPhoneSpan& span : phone_spans) {
    const CtcAlignmentToken& token = tokens[span.token_index];
    double& slot = vowel_start[token.word_index];
    if (is_vowel(token.phone) &&
        (slot < 0.0 || span.start_seconds < slot)) {
      slot = span.start_seconds;
    }
  }
  // The resumption scan is bounded by the next word's acoustic onset —
  // the rise ending the quiet run before its span — so a rise belonging
  // to that word's approach cannot be claimed. The decoder's own interval
  // end stays a lower bound — a packed-early decoder end must not block
  // a legitimate resumption inside the span.
  const auto scan_limit = [&](std::size_t i) {
    const double decoder_end =
        decoder_end_seconds.size() > i && decoder_end_seconds[i] > 0.0
            ? decoder_end_seconds[i]
            : words[i].end_seconds;
    if (i + 1 >= words.size()) {
      return std::max(decoder_end, words[i].end_seconds);
    }
    const DipEdge next_onset =
        gap_dip(grid, words[i].end_seconds, words[i + 1].start_seconds);
    const double claimed =
        (next_onset.found ? next_onset.edge : words[i + 1].start_seconds) -
        kResumeScanMarginSeconds;
    return std::max(decoder_end, claimed);
  };
  starts.push_back(lead_back_onset(
      resumption_edge(grid, words[0].start_seconds, words[0].end_seconds,
                      vowel_start[0], scan_limit(0)),
      words[0].start_seconds));
  const bool debug = std::getenv("SVP_BOUNDARY_DEBUG") != nullptr;
  for (std::size_t i = 1; i < words.size(); ++i) {
    const DipEdge resumed =
        resumption_edge(grid, words[i].start_seconds, words[i].end_seconds,
                        vowel_start[i], scan_limit(i));
    const double cur_start = words[i].start_seconds;
    const double prev_end = words[i - 1].end_seconds;
    const double gap = cur_start - prev_end;
    double start = cur_start;
    const char* rule = "phones";
    DipEdge dip;
    bool decided = false;
    if (resumed.found) {
      start = lead_back_onset(resumed, cur_start);
      rule = "resumption";
      dip = resumed;
      decided = true;
    } else if (gap >= kSilenceGapSeconds) {
      dip = onset_edge(grid, prev_end, cur_start);
      start = dip.found ? lead_back_onset(dip, cur_start) : cur_start;
      rule = dip.found ? "onset" : "onset-miss";
      decided = true;
    } else {
      dip = gap_dip(grid, prev_end, cur_start);
      if (dip.found) {
        start = lead_back_onset(dip, cur_start);
        rule = "dip";
        decided = true;
      } else if (gap >= kGapMidMinSeconds) {
        start = prev_end + kValleyBoundaryFraction * gap;
        rule = "midpoint";
        decided = true;
      }
    }
    if (debug) {
      std::fprintf(stderr,
                   "w=%zu span=[%.3f,%.3f] gap=%.3f rule=%s dip=[%.3f,%.3f] "
                   "out=%.3f\n",
                   i, cur_start, words[i].end_seconds, gap, rule,
                   dip.found ? dip.start : -1.0, dip.found ? dip.edge : -1.0,
                   start);
    }
    if (decided) {
      starts.push_back(start);
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
