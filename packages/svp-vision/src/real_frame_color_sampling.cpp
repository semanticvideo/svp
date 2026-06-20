#include "svp/vision/real_frame_color_sampling.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/foundation_color_staging.hpp"

#include <iomanip>
#include <sstream>

namespace svp::vision {
namespace {

std::vector<std::string> frame_ids_from_frames(
    const std::vector<ColorRasterFrame>& frames) {
  std::vector<std::string> ids;
  ids.reserve(frames.size());
  for (const ColorRasterFrame& f : frames) {
    ids.push_back(f.frame_id);
  }
  return ids;
}

}  // namespace

RealFrameSamplingResult build_real_frame_color_sampling_input(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path) {
  RealFrameSamplingResult result;

  const DecodedCanonicalFrames decoded =
      decode_canonical_frames(plan, ffmpeg_path);

  result.real_decoding_attempted = decoded.decoding_attempted;
  result.real_decoding_succeeded = decoded.decoding_succeeded;
  result.frames_attempted = decoded.frames_attempted;
  result.frames_decoded = decoded.frames_decoded;
  result.frames_missed = decoded.frames_missed;
  result.skipped_reason = decoded.skipped_reason;

  if (!decoded.decoding_succeeded) {
    result.input = build_foundation_color_staging_sample_input();
    return result;
  }

  // Build scenes and shots from the decoded frames.
  // One scene spanning all decoded frames; one shot per adjacent frame pair
  // (or a single shot if only one frame decoded).
  std::vector<std::int64_t> decoded_timestamps;
  decoded_timestamps.reserve(decoded.frames.size());
  for (const ColorRasterFrame& f : decoded.frames) {
    decoded_timestamps.push_back(f.timestamp_us);
  }

  const std::vector<std::string> all_frame_ids =
      frame_ids_from_frames(decoded.frames);

  ColorTimelineRange full_scene;
  full_scene.target_id = "scene_000001";
  full_scene.start_us = decoded_timestamps.front();
  full_scene.end_us = decoded_timestamps.back();
  full_scene.frame_ids = all_frame_ids;

  std::vector<ColorTimelineRange> shots;
  if (decoded.frames.size() == 1) {
    ColorTimelineRange shot;
    shot.target_id = "shot_000001";
    shot.start_us = decoded_timestamps.front();
    shot.end_us = decoded_timestamps.front();
    shot.frame_ids = {decoded.frames[0].frame_id};
    shots.push_back(std::move(shot));
  } else {
    for (std::size_t i = 0; i + 1 < decoded_timestamps.size(); ++i) {
      ColorTimelineRange shot;
      std::ostringstream id_oss;
      id_oss << "shot_" << std::setw(6) << std::setfill('0') << (i + 1);
      shot.target_id = id_oss.str();
      shot.start_us = decoded_timestamps[i];
      shot.end_us = decoded_timestamps[i + 1];
      shot.frame_ids = {decoded.frames[i].frame_id, decoded.frames[i + 1].frame_id};
      shots.push_back(std::move(shot));
    }
  }

  result.real_decoding_succeeded = true;
  result.input = ColorFrameSamplingInput{
      std::move(decoded.frames),
      {std::move(full_scene)},
      std::move(shots),
  };
  return result;
}

}  // namespace svp::vision
