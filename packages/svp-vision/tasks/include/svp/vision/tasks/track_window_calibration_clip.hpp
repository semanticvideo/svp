#pragma once

// The fixed calibration window tracking capacity is measured on (plan §3.5:
// each Mac measures track.window throughput at increasing slot counts). It is
// a short synthetic clip every Mac decodes and tracks the same way, so
// measurements of different Macs compare like for like.
//
// Content: a raster of the largest canonical analysis size for 16:9 sources
// (kCanonicalLongestDisplayDimension wide), covered by tiles of unrelated
// colours, with textured objects moving across it, so every stage of a
// window has work: decode, the detector on every frame, depth on the
// scheduled frames, dense optical flow and motion clustering on every pair.
// One frame per sample interval of the build's tracking quality, so depth is
// scheduled as the build schedules it. Frames are lossless FFV1, every frame
// a key frame, so a seek lands on exactly the planned frame.

#include "svp/media/canonical_raster.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace svp::vision::tasks {

// Bumped whenever the clip's content or encoding changes; calibration
// records name it so a changed clip invalidates old measurements.
inline constexpr std::uint32_t kTrackWindowCalibrationRecipeVersion = 1;

inline constexpr int kTrackWindowCalibrationFrameWidth =
    svp::media::kCanonicalLongestDisplayDimension;
// 16:9 at that width, the most common canonical raster.
inline constexpr int kTrackWindowCalibrationFrameHeight =
    svp::media::kCanonicalLongestDisplayDimension * 9 / 16;
// Twelve frames: a sweep step takes seconds, yet holds several depth frames
// at every quality's depth cadence (every 2nd to 5th frame) and enough
// frame pairs for the tracker's per-pair work to dominate its setup.
inline constexpr std::size_t kTrackWindowCalibrationFrames = 12;

// The clip's frame timestamps for a quality whose sample interval is
// `sample_interval_us`, known without writing it.
[[nodiscard]] std::vector<std::int64_t> track_window_calibration_timestamps_us(
    std::int64_t sample_interval_us);

// Renders the clip with OpenCV and encodes it with `ffmpeg` into
// `directory/track-window-calibration.mkv`, one frame per
// `sample_interval_us`. Throws std::runtime_error when ffmpeg fails.
[[nodiscard]] std::filesystem::path write_track_window_calibration_clip(
    const std::filesystem::path& ffmpeg, const std::filesystem::path& directory,
    std::int64_t sample_interval_us);

}  // namespace svp::vision::tasks
