#include "svp/vision/color_frame_sampling.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace svp::vision {
namespace {

void require_id(const std::string& id, const std::string& owner) {
  if (id.empty()) {
    throw std::invalid_argument(owner + " id must not be empty");
  }
}

void validate_frame(const ColorRasterFrame& frame) {
  require_id(frame.frame_id, "frame");
  if (frame.width <= 0 || frame.height <= 0) {
    throw std::invalid_argument("color raster frame dimensions must be positive");
  }
  const auto expected_pixel_count =
      static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
  if (frame.pixels.size() != expected_pixel_count) {
    throw std::invalid_argument(
        "color raster frame pixel count must match dimensions");
  }
}

std::map<std::string, ColorRasterFrame> index_frames(
    const std::vector<ColorRasterFrame>& frames) {
  std::map<std::string, ColorRasterFrame> frames_by_id;
  for (const ColorRasterFrame& frame : frames) {
    validate_frame(frame);
    const auto [_, inserted] = frames_by_id.emplace(frame.frame_id, frame);
    if (!inserted) {
      throw std::invalid_argument("duplicate color raster frame id: " +
                                  frame.frame_id);
    }
  }
  return frames_by_id;
}

void validate_range(const ColorTimelineRange& range,
                    const std::string& target_type,
                    const std::map<std::string, ColorRasterFrame>& frames_by_id) {
  require_id(range.target_id, target_type);
  if (range.end_us < range.start_us) {
    throw std::invalid_argument(target_type + " color range end precedes start");
  }
  if (range.frame_ids.empty()) {
    throw std::invalid_argument(target_type +
                                " color range must reference at least one frame");
  }

  std::set<std::string> seen_frame_ids;
  for (const std::string& frame_id : range.frame_ids) {
    require_id(frame_id, "sampled frame reference");
    if (!seen_frame_ids.insert(frame_id).second) {
      throw std::invalid_argument(target_type +
                                  " color range repeats a frame reference");
    }
    if (!frames_by_id.contains(frame_id)) {
      throw std::invalid_argument(target_type +
                                  " color range references missing frame: " +
                                  frame_id);
    }
  }
}

std::vector<std::string> keyframe_ids_for_range(
    const ColorTimelineRange& range,
    const std::map<std::string, ColorRasterFrame>& frames_by_id) {
  std::vector<std::string> selected_frame_ids;
  for (const std::string& frame_id : range.frame_ids) {
    if (frames_by_id.at(frame_id).keyframe) {
      selected_frame_ids.push_back(frame_id);
    }
  }
  if (selected_frame_ids.empty()) {
    selected_frame_ids = range.frame_ids;
  }
  return selected_frame_ids;
}

std::vector<Srgb8Pixel> pixels_for_frames(
    const std::vector<std::string>& frame_ids,
    const std::map<std::string, ColorRasterFrame>& frames_by_id) {
  std::vector<Srgb8Pixel> pixels;
  for (const std::string& frame_id : frame_ids) {
    const std::vector<Srgb8Pixel>& frame_pixels = frames_by_id.at(frame_id).pixels;
    pixels.insert(pixels.end(), frame_pixels.begin(), frame_pixels.end());
  }
  return pixels;
}

void add_reference(ColorObservationTargetReferences& references,
                   const std::string& target_type,
                   const std::string& target_id) {
  references.ids_by_target_type[target_type].push_back(target_id);
}

void sort_references(ColorObservationTargetReferences& references) {
  for (auto& [_, ids] : references.ids_by_target_type) {
    std::sort(ids.begin(), ids.end());
  }
}

void append_frame_summaries(
    const std::map<std::string, ColorRasterFrame>& frames_by_id,
    SampledColorObservationPlan& plan) {
  for (const auto& [frame_id, frame] : frames_by_id) {
    add_reference(plan.target_references, "frame", frame_id);
    plan.summaries.push_back(summarize_color_samples(
        ColorObservationTarget{
            "frame",
            frame_id,
            frame.timestamp_us,
            frame.timestamp_us,
            {frame_id},
            "full_frame",
        },
        frame.pixels));
  }
}

void append_range_summaries(
    const std::string& target_type,
    const std::vector<ColorTimelineRange>& ranges,
    const std::map<std::string, ColorRasterFrame>& frames_by_id,
    SampledColorObservationPlan& plan) {
  for (const ColorTimelineRange& range : ranges) {
    validate_range(range, target_type, frames_by_id);
    add_reference(plan.target_references, target_type, range.target_id);
    const std::vector<std::string> sampled_frame_ids =
        keyframe_ids_for_range(range, frames_by_id);
    plan.summaries.push_back(summarize_color_samples(
        ColorObservationTarget{
            target_type,
            range.target_id,
            range.start_us,
            range.end_us,
            sampled_frame_ids,
            "keyframe_full_frame",
        },
        pixels_for_frames(sampled_frame_ids, frames_by_id)));
  }
}

}  // namespace

SampledColorObservationPlan sample_frame_scene_shot_colors(
    const ColorFrameSamplingInput& input) {
  const std::map<std::string, ColorRasterFrame> frames_by_id =
      index_frames(input.frames);
  if (frames_by_id.empty()) {
    throw std::invalid_argument("color frame sampling requires at least one frame");
  }

  SampledColorObservationPlan plan;
  plan.summaries.reserve(frames_by_id.size() + input.scenes.size() +
                         input.shots.size());

  append_frame_summaries(frames_by_id, plan);
  append_range_summaries("scene", input.scenes, frames_by_id, plan);
  append_range_summaries("shot", input.shots, frames_by_id, plan);
  sort_references(plan.target_references);
  return plan;
}

}  // namespace svp::vision
