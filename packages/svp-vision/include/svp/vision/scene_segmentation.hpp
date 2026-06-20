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
// dominant-bucket-change detection.
//
// Scene boundaries are placed where the dominant color bucket of a frame
// differs from the previous frame's dominant bucket by more than a threshold
// fraction of the frame's pixel coverage.  Each scene spans one or more
// consecutive frames with similar dominant color character.
//
// Shots are created one per frame within each scene (or per adjacent pair when
// a scene has more than one frame), giving shot-level color summaries that
// cover every sampled timestamp.
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
