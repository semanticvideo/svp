#include "ocr_generation_internal.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <map>
#include <numeric>
#include <tuple>

namespace svp::vision::ocr_generation_internal {
namespace {

double bbox_iou(int a_left,
                int a_top,
                int a_right,
                int a_bottom,
                int b_left,
                int b_top,
                int b_right,
                int b_bottom) {
  const int inter_left = std::max(a_left, b_left);
  const int inter_top = std::max(a_top, b_top);
  const int inter_right = std::min(a_right, b_right);
  const int inter_bottom = std::min(a_bottom, b_bottom);
  if (inter_right <= inter_left || inter_bottom <= inter_top) return 0.0;
  const double inter_area =
      static_cast<double>(inter_right - inter_left) *
      static_cast<double>(inter_bottom - inter_top);
  const double a_area =
      static_cast<double>(a_right - a_left) *
      static_cast<double>(a_bottom - a_top);
  const double b_area =
      static_cast<double>(b_right - b_left) *
      static_cast<double>(b_bottom - b_top);
  const double union_area = a_area + b_area - inter_area;
  return union_area > 0.0 ? inter_area / union_area : 0.0;
}

double bbox_iou(const FrameDetection& a, const FrameDetection& b) {
  return bbox_iou(a.bbox_left, a.bbox_top, a.bbox_right, a.bbox_bottom,
                  b.bbox_left, b.bbox_top, b.bbox_right, b.bbox_bottom);
}

// Readings of one line on screen differ when the recognizer clips a letter at
// the box edge or misreads one ("nvestor" for "investor", "evervbody" for
// "everybody"). Each different reading used to become its own observation with
// its own evidence crop, all of the same line. Readings in the same place within
// a few seconds whose text is near-identical are now one observation.
constexpr double kSamePlaceIou = 0.3;
constexpr std::int64_t kNearIdenticalWithinUs = 5'000'000;
constexpr double kNearIdenticalSimilarity = 0.8;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_number_mark(char c) { return c == '$' || c == ',' || c == '.'; }

std::string digits_of(const std::string& key) {
  std::string digits;
  for (char c : key) {
    if (is_digit(c)) digits += c;
  }
  return digits;
}

// The recognizer reads a digit as the letter it looks like ("$2.O65.512" for
// "$2,065,512", "Flips: 1O43" for "Flips: 1043"), so before readings are
// compared those letters are read back as digits, but only inside a number: a
// run of digits, number marks ($ , .) and look-alike letters with at least one
// digit. A run with any other letter is a word and keeps its letters ("SOLD",
// "4.Benjamin"); so does a run without a digit ("BOSS."). An S that starts a
// number stays: it is as often a misread $ ("S774" for "$774") as a 5.
char digit_lookalike(char c) {
  switch (c) {
    case 'O':
    case 'o':
      return '0';
    case 'I':
    case 'l':
      return '1';
    case 'S':
      return '5';
    case 'B':
      return '8';
    default:
      return '\0';
  }
}

std::string read_digit_lookalikes(std::string text) {
  for (std::size_t start = 0; start < text.size();) {
    std::size_t end = start;
    bool has_digit = false;
    bool is_word = false;
    for (; end < text.size(); ++end) {
      const char c = text[end];
      if (!is_digit(c) && !is_letter(c) && !is_number_mark(c)) break;
      has_digit = has_digit || is_digit(c);
      is_word = is_word || (is_letter(c) && digit_lookalike(c) == '\0');
    }
    if (has_digit && !is_word) {
      for (std::size_t i = start; i < end; ++i) {
        if (i == start && text[i] == 'S') continue;
        if (const char digit = digit_lookalike(text[i])) text[i] = digit;
      }
    }
    start = end + 1;
  }
  return text;
}

// 1 - edit distance / longer length, or 0 as soon as it can't reach `floor`.
double similarity(const std::string& a, const std::string& b, double floor) {
  const std::size_t longer = std::max(a.size(), b.size());
  if (longer == 0) return 1.0;
  const auto budget = static_cast<std::size_t>((1.0 - floor) * static_cast<double>(longer));
  if ((a.size() > b.size() ? a.size() - b.size() : b.size() - a.size()) > budget) return 0.0;
  std::vector<std::size_t> row(b.size() + 1);
  std::iota(row.begin(), row.end(), std::size_t{0});
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diagonal = row[0];
    row[0] = i;
    std::size_t row_min = row[0];
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t above = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diagonal = above;
      row_min = std::min(row_min, row[j]);
    }
    if (row_min > budget) return 0.0;
  }
  return 1.0 - static_cast<double>(row[b.size()]) / static_cast<double>(longer);
}

// Two readings of one line (as read_digit_lookalikes reads them): the same
// digits, and one is a clipped piece of the other or they differ by a few
// letters. A clipped piece often has its edge letters misread ("ouy this park"
// for "buy this park"), so up to kClippedEdgeChars at each end of it are
// ignored. Different digits never merge, so a changed number stays its own
// observation.
constexpr std::size_t kClippedEdgeChars = 2;

bool near_identical(const std::string& a, const std::string& b) {
  if (a == b) return true;
  if (digits_of(a) != digits_of(b)) return false;
  const std::string& shorter = a.size() <= b.size() ? a : b;
  const std::string& longer = a.size() <= b.size() ? b : a;
  for (std::size_t front = 0; front <= kClippedEdgeChars; ++front) {
    for (std::size_t back = 0; back <= kClippedEdgeChars; ++back) {
      if (front + back >= shorter.size()) continue;
      const std::string piece = shorter.substr(front, shorter.size() - front - back);
      if (piece.size() * 2 >= longer.size() && longer.find(piece) != std::string::npos) {
        return true;
      }
    }
  }
  return similarity(shorter, longer, kNearIdenticalSimilarity) >= kNearIdenticalSimilarity;
}

std::size_t find_root(std::vector<std::size_t>& parent, std::size_t i) {
  while (parent[i] != i) {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}

}  // namespace

std::vector<ReconciledObservation> reconcile_detections(
    const std::vector<FrameDetection>& detections,
    int total_frames,
    double min_confidence,
    std::size_t min_text_chars) {
  std::vector<ReconciledObservation> reconciled;

  std::vector<std::string> keys(detections.size());
  std::vector<std::string> compared_keys(detections.size());
  std::map<std::string, std::vector<std::size_t>> by_text_key;
  for (std::size_t i = 0; i < detections.size(); ++i) {
    const std::string norm = normalize_text(detections[i].raw_text);
    keys[i] = alphanumeric_key(norm);
    if (keys[i].length() < min_text_chars) continue;
    by_text_key[keys[i]].push_back(i);
    compared_keys[i] =
        alphanumeric_key(normalize_text(read_digit_lookalikes(detections[i].raw_text)));
  }

  // The same text in the same place is one observation, whenever it was seen.
  std::vector<std::size_t> parent(detections.size());
  std::iota(parent.begin(), parent.end(), std::size_t{0});
  std::vector<std::size_t> kept;
  for (const auto& [key, indices] : by_text_key) {
    std::vector<std::vector<std::size_t>> places;  // each place this text was seen
    for (std::size_t idx : indices) {
      kept.push_back(idx);
      bool added = false;
      for (auto& place : places) {
        for (std::size_t member : place) {
          if (bbox_iou(detections[idx], detections[member]) > kSamePlaceIou) {
            parent[idx] = place.front();
            place.push_back(idx);
            added = true;
            break;
          }
        }
        if (added) break;
      }
      if (!added) places.push_back({idx});
    }
  }

  // Near-identical readings in the same place within a few seconds join it.
  std::sort(kept.begin(), kept.end(), [&](std::size_t a, std::size_t b) {
    return std::tie(detections[a].timestamp_us, a) < std::tie(detections[b].timestamp_us, b);
  });
  for (std::size_t i = 0; i < kept.size(); ++i) {
    const FrameDetection& first = detections[kept[i]];
    for (std::size_t j = i + 1; j < kept.size(); ++j) {
      const FrameDetection& later = detections[kept[j]];
      if (later.timestamp_us - first.timestamp_us > kNearIdenticalWithinUs) break;
      const std::size_t a = find_root(parent, kept[i]);
      const std::size_t b = find_root(parent, kept[j]);
      if (a == b || bbox_iou(first, later) <= kSamePlaceIou) continue;
      if (near_identical(compared_keys[kept[i]], compared_keys[kept[j]])) {
        parent[std::max(a, b)] = std::min(a, b);
      }
    }
  }

  std::map<std::size_t, std::vector<std::size_t>> groups;  // by root: deterministic order
  std::sort(kept.begin(), kept.end());
  for (std::size_t idx : kept) groups[find_root(parent, idx)].push_back(idx);

  for (const auto& [root, cluster] : groups) {
    {
      ReconciledObservation obs;
      // The reading seen most often (then the longest) names the observation,
      // in its fullest spelling.
      std::map<std::string, std::size_t> reads;
      for (std::size_t idx : cluster) ++reads[keys[idx]];
      const auto named = std::max_element(reads.begin(), reads.end(),
          [](const auto& a, const auto& b) {
            return std::make_pair(a.second, a.first.size()) <
                   std::make_pair(b.second, b.first.size());
          });
      std::size_t best_idx = cluster[0];
      bool found = false;
      for (std::size_t idx : cluster) {
        if (keys[idx] != named->first) continue;
        if (!found || detections[idx].raw_text.size() > detections[best_idx].raw_text.size()) {
          best_idx = idx;
          found = true;
        }
      }
      obs.raw_text = detections[best_idx].raw_text;
      obs.normalized_text = normalize_text(obs.raw_text);
      obs.frame_width = detections[cluster[0]].frame_width;
      obs.frame_height = detections[cluster[0]].frame_height;
      obs.detection_count = static_cast<int>(cluster.size());

      // Confidence comes from recognitions only: a reused detection repeats
      // an earlier frame's recognition and adds no reading of its own.
      double conf_sum = 0.0;
      double reused_conf_sum = 0.0;
      std::size_t recognized = 0;
      obs.bbox_left = INT_MAX;
      obs.bbox_top = INT_MAX;
      obs.bbox_right = 0;
      obs.bbox_bottom = 0;
      obs.start_us = INT64_MAX;
      obs.end_us = 0;
      obs.frame_start = SIZE_MAX;
      obs.frame_end = 0;

      for (std::size_t idx : cluster) {
        const auto& det = detections[idx];
        if (det.reused) {
          reused_conf_sum += det.confidence;
        } else {
          conf_sum += det.confidence;
          ++recognized;
        }
        obs.bbox_left = std::min(obs.bbox_left, det.bbox_left);
        obs.bbox_top = std::min(obs.bbox_top, det.bbox_top);
        obs.bbox_right = std::max(obs.bbox_right, det.bbox_right);
        obs.bbox_bottom = std::max(obs.bbox_bottom, det.bbox_bottom);
        obs.start_us = std::min(obs.start_us, det.timestamp_us);
        obs.end_us = std::max(obs.end_us, det.timestamp_us);
        obs.frame_start = std::min(obs.frame_start, det.frame_index);
        obs.frame_end = std::max(obs.frame_end, det.frame_index);
        obs.source_frame_ids.push_back(det.frame_id);
      }

      // A cluster of only reused detections (their recognition clustered
      // elsewhere) still holds one reading: the confidence they copied.
      const std::size_t readings = std::max<std::size_t>(1, recognized);
      obs.confidence = recognized > 0
                           ? conf_sum / static_cast<double>(recognized)
                           : reused_conf_sum / static_cast<double>(cluster.size());
      if (total_frames > 1 && readings == 1 && obs.confidence < min_confidence) {
        continue;
      }
      if (readings > 1) {
        obs.confidence = std::min(1.0, obs.confidence + 0.1 * (readings - 1));
      }
      reconciled.push_back(std::move(obs));
    }
  }

  std::sort(reconciled.begin(), reconciled.end(),
            [](const ReconciledObservation& a, const ReconciledObservation& b) {
              return a.start_us < b.start_us;
            });

  return reconciled;
}

}  // namespace svp::vision::ocr_generation_internal
