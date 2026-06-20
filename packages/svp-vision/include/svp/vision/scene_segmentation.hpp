#pragma once

#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/color_quantization.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace svp::vision {

// A scene boundary candidate produced by deterministic color-change analysis.
struct SceneSegment {
  std::string scene_id;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::vector<std::string> frame_ids;
  std::string dominant_bucket;
};

// Result of deterministic scene/shot segmentation from decoded frames.
struct SceneSegmentationResult {
  std::vector<SceneSegment> scenes;
  std::vector<ColorTimelineRange> shots;
  std::string method;
};

// Segment decoded frames into scenes and shots using deterministic
// color-distribution-change detection.
//
// Scene boundaries are placed where any of the following signals fire
// between consecutive frames:
//   - The dominant color bucket changes.
//   - The L1 distance between full bucket-coverage vectors exceeds a
//     threshold (catches distribution shifts even when the dominant
//     bucket stays the same).
//   - The dominant bucket's coverage fraction jumps sharply (catches
//     transitions from mixed to near-solid color).
//   - The color diversity (count of non-gray buckets above 5%) changes
//     by 2 or more (catches warm-gray vs gray/multicolor splits).
//
// Each scene spans one or more consecutive frames with similar color
// distribution character.  Shots are created one per frame.
//
// This is a foundation heuristic — it does not claim semantic scene
// understanding.  It honestly describes its limits via the method string.
//
// min_scene_frames: scenes with fewer than this many frames are merged into
//   the previous scene (prevents over-fragmentation from single-frame spikes).
//   Set to 1 to disable merging.
[[nodiscard]] SceneSegmentationResult segment_frames_by_color_change(
    const std::vector<ColorRasterFrame>& frames,
    int min_scene_frames = 1);

}  // namespace svp::vision
