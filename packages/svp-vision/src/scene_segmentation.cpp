#include "svp/vision/scene_segmentation.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace svp::vision {
namespace {

// L1 distance threshold for distribution-based scene splits.  When the L1
// distance between consecutive frame bucket-coverage vectors exceeds this
// value, a scene boundary is placed even if the dominant bucket is unchanged.
// The mixed-red object scene (~30% red, ~18% pink, ~15% purple) vs the
// near-solid hot-pink/red block (~98% red) produces an L1 distance well
// above 1.0, so 0.5 catches that transition while avoiding noise from minor
// frame-to-frame jitter.
constexpr double kL1DistanceThreshold = 0.5;

// When the dominant bucket's coverage jumps by more than this fraction between
// consecutive frames, place a scene boundary.  This catches transitions where
// the dominant bucket stays the same but its concentration changes sharply
// (e.g. red going from 30% to 97%).
constexpr double kDominantCoverageJumpThreshold = 0.35;

// Minimum change in color diversity (count of non-gray, non-black buckets with
// coverage >= 5%) to force a scene boundary even when L1 distance is moderate.
constexpr int kDiversityChangeThreshold = 2;

// Minimum coverage for a bucket to count toward color diversity.
constexpr double kDiversityBucketCoverage = 0.05;

// Compute the full bucket coverage vector for a frame.
std::map<std::string, double> bucket_coverage_vector(
    const ColorRasterFrame& frame) {
  std::map<std::string, double> coverage;
  for (const std::string& id : registered_color_bucket_ids()) {
    coverage[id] = 0.0;
  }
  if (frame.pixels.empty()) {
    return coverage;
  }
  for (const Srgb8Pixel& pixel : frame.pixels) {
    const std::string bucket =
        assign_registered_color_bucket(srgb8_to_oklch(pixel));
    coverage[bucket] += 1.0;
  }
  const double total = static_cast<double>(frame.pixels.size());
  for (auto& [_, value] : coverage) {
    value /= total;
  }
  return coverage;
}

// Compute the dominant color bucket from a coverage vector.
std::string dominant_bucket_from_coverage(
    const std::map<std::string, double>& coverage) {
  const auto max_it = std::max_element(
      coverage.begin(), coverage.end(),
      [](const auto& a, const auto& b) { return a.second < b.second; });
  if (max_it == coverage.end()) {
    return "other";
  }
  return max_it->first;
}

// Compute the dominant color bucket for a frame by quantizing every pixel.
std::string dominant_bucket_for_frame(const ColorRasterFrame& frame) {
  return dominant_bucket_from_coverage(bucket_coverage_vector(frame));
}

// L1 distance between two coverage vectors (sum of absolute differences).
double l1_distance(const std::map<std::string, double>& a,
                   const std::map<std::string, double>& b) {
  double dist = 0.0;
  for (const std::string& id : registered_color_bucket_ids()) {
    dist += std::fabs(a.at(id) - b.at(id));
  }
  return dist;
}

// Count non-gray, non-black buckets with coverage >= threshold.
int color_diversity(const std::map<std::string, double>& coverage) {
  int count = 0;
  for (const auto& [bucket, value] : coverage) {
    if (bucket == "gray" || bucket == "black" || bucket == "white") {
      continue;
    }
    if (value >= kDiversityBucketCoverage) {
      ++count;
    }
  }
  return count;
}

std::string make_scene_id(int index) {
  std::ostringstream oss;
  oss << "scene_" << std::setw(6) << std::setfill('0') << (index + 1);
  return oss.str();
}

std::string make_shot_id(int index) {
  std::ostringstream oss;
  oss << "shot_" << std::setw(6) << std::setfill('0') << (index + 1);
  return oss.str();
}

// A raw scene boundary group: consecutive frames with similar color
// distribution.
struct RawSegment {
  std::size_t begin_index;
  std::size_t end_index;  // inclusive
  std::string dominant_bucket;
};

// Merge raw segments that have fewer than min_scene_frames into the previous
// segment.  If the first segment is too short, it merges into the next one.
std::vector<RawSegment> merge_short_segments(
    std::vector<RawSegment> segments,
    int min_scene_frames) {
  if (segments.empty() || min_scene_frames <= 1) {
    return segments;
  }

  std::vector<RawSegment> merged;
  for (RawSegment& seg : segments) {
    const int frame_count =
        static_cast<int>(seg.end_index - seg.begin_index + 1);
    if (frame_count < min_scene_frames && !merged.empty()) {
      // Merge into previous segment
      merged.back().end_index = seg.end_index;
    } else {
      merged.push_back(std::move(seg));
    }
  }

  // If the first segment is too short and there's a second, merge forward.
  if (merged.size() >= 2) {
    const int first_count =
        static_cast<int>(merged[0].end_index - merged[0].begin_index + 1);
    if (first_count < min_scene_frames) {
      merged[1].begin_index = merged[0].begin_index;
      merged.erase(merged.begin());
    }
  }

  return merged;
}

}  // namespace

SceneSegmentationResult segment_frames_by_color_change(
    const std::vector<ColorRasterFrame>& frames,
    int min_scene_frames) {
  if (frames.empty()) {
    throw std::invalid_argument(
        "scene segmentation requires at least one frame");
  }

  SceneSegmentationResult result;
  result.method = "deterministic_color_distribution_change_v2";

  if (frames.size() == 1) {
    SceneSegment seg;
    seg.scene_id = make_scene_id(0);
    seg.start_us = frames[0].timestamp_us;
    seg.end_us = frames[0].timestamp_us;
    seg.frame_ids = {frames[0].frame_id};
    seg.dominant_bucket = dominant_bucket_for_frame(frames[0]);
    result.scenes.push_back(std::move(seg));

    ColorTimelineRange shot;
    shot.target_id = make_shot_id(0);
    shot.start_us = frames[0].timestamp_us;
    shot.end_us = frames[0].timestamp_us;
    shot.frame_ids = {frames[0].frame_id};
    result.shots.push_back(std::move(shot));
    return result;
  }

  // Step 1: Compute coverage vectors and derived signals for each frame.
  std::vector<std::map<std::string, double>> coverages;
  coverages.reserve(frames.size());
  std::vector<std::string> dominant_buckets;
  dominant_buckets.reserve(frames.size());
  std::vector<int> diversities;
  diversities.reserve(frames.size());
  for (const ColorRasterFrame& frame : frames) {
    auto cov = bucket_coverage_vector(frame);
    diversities.push_back(color_diversity(cov));
    dominant_buckets.push_back(dominant_bucket_from_coverage(cov));
    coverages.push_back(std::move(cov));
  }

  // Step 2: Detect scene boundaries using multiple signals.
  // A boundary is placed between frame i-1 and frame i when any of:
  //   (a) dominant bucket changes
  //   (b) L1 distance between coverage vectors exceeds threshold
  //   (c) dominant bucket coverage jumps by more than threshold
  //   (d) color diversity changes by >= threshold
  std::vector<RawSegment> raw_segments;
  std::size_t seg_start = 0;
  for (std::size_t i = 1; i < frames.size(); ++i) {
    bool boundary = false;

    // (a) Dominant bucket change
    if (dominant_buckets[i] != dominant_buckets[i - 1]) {
      boundary = true;
    }

    // (b) L1 distance between consecutive coverage vectors
    if (!boundary) {
      const double dist = l1_distance(coverages[i], coverages[i - 1]);
      if (dist > kL1DistanceThreshold) {
        boundary = true;
      }
    }

    // (c) Dominant bucket coverage jump (same dominant, very different
    // concentration — e.g. red going from 30% to 97%)
    if (!boundary) {
      const std::string& dom = dominant_buckets[i - 1];
      const double prev_dom_cov = coverages[i - 1].at(dom);
      const double curr_dom_cov = coverages[i].at(dom);
      if (std::fabs(curr_dom_cov - prev_dom_cov) >
          kDominantCoverageJumpThreshold) {
        boundary = true;
      }
    }

    // (d) Color diversity change
    if (!boundary) {
      const int div_change =
          std::abs(diversities[i] - diversities[i - 1]);
      if (div_change >= kDiversityChangeThreshold) {
        boundary = true;
      }
    }

    if (boundary) {
      raw_segments.push_back(
          {seg_start, i - 1, dominant_buckets[seg_start]});
      seg_start = i;
    }
  }
  raw_segments.push_back(
      {seg_start, frames.size() - 1, dominant_buckets[seg_start]});

  // Step 3: Merge short segments to avoid over-fragmentation.
  const std::vector<RawSegment> merged_segments =
      merge_short_segments(std::move(raw_segments), min_scene_frames);

  // Step 4: Build scenes and shots from merged segments.
  int shot_counter = 0;
  for (std::size_t s = 0; s < merged_segments.size(); ++s) {
    const RawSegment& raw = merged_segments[s];

    SceneSegment seg;
    seg.scene_id = make_scene_id(static_cast<int>(s));
    seg.start_us = frames[raw.begin_index].timestamp_us;
    seg.end_us = frames[raw.end_index].timestamp_us;
    seg.dominant_bucket = raw.dominant_bucket;

    for (std::size_t i = raw.begin_index; i <= raw.end_index; ++i) {
      seg.frame_ids.push_back(frames[i].frame_id);
    }
    result.scenes.push_back(std::move(seg));

    // Create one shot per frame within the scene.
    for (std::size_t i = raw.begin_index; i <= raw.end_index; ++i) {
      ColorTimelineRange shot;
      shot.target_id = make_shot_id(shot_counter);
      shot.start_us = frames[i].timestamp_us;
      shot.end_us = frames[i].timestamp_us;
      shot.frame_ids = {frames[i].frame_id};
      result.shots.push_back(std::move(shot));
      ++shot_counter;
    }
  }

  return result;
}

}  // namespace svp::vision
