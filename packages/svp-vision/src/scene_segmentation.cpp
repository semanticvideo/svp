#include "svp/vision/scene_segmentation.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace svp::vision {
namespace {

// Compute the dominant color bucket for a frame by quantizing every pixel.
std::string dominant_bucket_for_frame(const ColorRasterFrame& frame) {
  if (frame.pixels.empty()) {
    return "other";
  }

  std::map<std::string, int> bucket_counts;
  for (const Srgb8Pixel& pixel : frame.pixels) {
    const std::string bucket =
        assign_registered_color_bucket(srgb8_to_oklch(pixel));
    bucket_counts[bucket]++;
  }

  const auto max_it =
      std::max_element(bucket_counts.begin(), bucket_counts.end(),
                       [](const auto& a, const auto& b) {
                         return a.second < b.second;
                       });
  return max_it->first;
}

// Compute the coverage fraction of a specific bucket in a frame.
double bucket_coverage_for_frame(const ColorRasterFrame& frame,
                                 const std::string& bucket_id) {
  if (frame.pixels.empty()) {
    return 0.0;
  }

  int count = 0;
  for (const Srgb8Pixel& pixel : frame.pixels) {
    if (assign_registered_color_bucket(srgb8_to_oklch(pixel)) == bucket_id) {
      ++count;
    }
  }
  return static_cast<double>(count) /
         static_cast<double>(frame.pixels.size());
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

// A raw scene boundary group: consecutive frames sharing the same dominant
// bucket.
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
  result.method = "deterministic_dominant_bucket_change_v1";

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

  // Step 1: Compute dominant bucket for each frame.
  std::vector<std::string> dominant_buckets;
  dominant_buckets.reserve(frames.size());
  for (const ColorRasterFrame& frame : frames) {
    dominant_buckets.push_back(dominant_bucket_for_frame(frame));
  }

  // Step 2: Group consecutive frames by dominant bucket into raw segments.
  // A scene boundary is placed when the dominant bucket changes between
  // consecutive frames.
  std::vector<RawSegment> raw_segments;
  std::size_t seg_start = 0;
  for (std::size_t i = 1; i < frames.size(); ++i) {
    if (dominant_buckets[i] != dominant_buckets[seg_start]) {
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
