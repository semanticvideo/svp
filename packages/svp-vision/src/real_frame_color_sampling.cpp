#include "svp/vision/real_frame_color_sampling.hpp"

#include "svp/vision/foundation_color_staging.hpp"
#include "svp/media/canonical_timing.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>

namespace svp::vision {
namespace {

// Maximum number of canonical frames to decode per build run.
// Kept small so the foundation-color stage finishes quickly and
// the real/synthetic difference is immediately observable.
constexpr int kMaxDecodedFrames = 5;

// A seek that lands within this margin of the end of the file is clamped
// back so ffmpeg does not seek past the last decodable frame.
constexpr std::int64_t kEndMarginUs = 100000;  // 100 ms

bool ffmpeg_is_available(const std::filesystem::path& ffmpeg_path) {
  if (ffmpeg_path.empty()) {
    return false;
  }
  if (ffmpeg_path.has_parent_path()) {
    return std::filesystem::exists(ffmpeg_path);
  }

  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return false;
  }

  std::string paths(path_env);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t end = paths.find(':', start);
    const std::string entry =
        paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!entry.empty() &&
        std::filesystem::exists(std::filesystem::path(entry) / ffmpeg_path)) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

// Shell-quote a path for embedding in a popen command string.
std::string shell_quote(const std::filesystem::path& path) {
  std::string quoted = "'";
  for (const char c : path.string()) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

// Format a microsecond timestamp as an ffmpeg seek position ("ss.ffffff").
std::string microseconds_to_seek_string(std::int64_t us) {
  const std::int64_t seconds = us / 1000000;
  const std::int64_t fraction = us % 1000000;
  std::ostringstream oss;
  oss << seconds << "." << std::setw(6) << std::setfill('0') << fraction;
  return oss.str();
}

// Deterministic frame ID from zero-based index.
std::string frame_id(int index) {
  std::ostringstream oss;
  oss << "frame_" << std::setw(6) << std::setfill('0') << (index + 1);
  return oss.str();
}

// Decode exactly one frame at seek_us into rgb24 rawvideo at (width x height).
// Returns the pixel buffer on success; returns empty on any decode failure.
// Uses popen so it works without linking libav directly.
std::vector<Srgb8Pixel> decode_frame_at(const std::filesystem::path& ffmpeg_path,
                                         const std::filesystem::path& source_path,
                                         std::int64_t seek_us,
                                         int width,
                                         int height,
                                         std::string& out_error) {
  const std::string seek = microseconds_to_seek_string(seek_us);

  // -ss before -i performs a fast keyframe seek; the subsequent scale filter
  // resizes the decoded frame to the exact canonical raster dimensions.
  const std::string cmd =
      shell_quote(ffmpeg_path) +
      " -v error"
      " -ss " + seek +
      " -i " + shell_quote(source_path) +
      " -vf scale=" + std::to_string(width) + ":" + std::to_string(height) +
      " -vframes 1"
      " -f rawvideo"
      " -pix_fmt rgb24"
      " pipe:1"
      " 2>/dev/null";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    out_error = std::string("popen failed: ") + std::strerror(errno);
    return {};
  }

  const std::size_t expected_bytes =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
  std::vector<std::uint8_t> raw_bytes(expected_bytes);
  const std::size_t bytes_read = std::fread(raw_bytes.data(), 1, expected_bytes, pipe);

  const int status = pclose(pipe);
  const bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (bytes_read != expected_bytes) {
    // Incomplete pixel buffer: ffmpeg may have exited 0 but seeked past the
    // last decodable frame in a short file.  Treat as a miss for this
    // timestamp rather than a hard failure of the whole run.
    out_error = "short read: got " + std::to_string(bytes_read) + "/" +
                std::to_string(expected_bytes) + " bytes (ffmpeg exit " +
                std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status) + ")";
    return {};
  }

  if (!exited_ok) {
    out_error = "ffmpeg exited with status " +
                std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status);
    return {};
  }

  std::vector<Srgb8Pixel> pixels;
  pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for (std::size_t i = 0; i < raw_bytes.size(); i += 3) {
    pixels.push_back({raw_bytes[i], raw_bytes[i + 1], raw_bytes[i + 2]});
  }
  return pixels;
}

// Compute deterministic seek timestamps distributed across the video duration.
// Timestamps are placed at midpoints of N equal intervals so they stay well
// away from both ends.  The last timestamp is further clamped to at most
// (duration_us - kEndMarginUs) so short or generated videos do not produce
// seeks past the last decodable keyframe.
std::vector<std::int64_t> deterministic_seek_timestamps_us(
    std::int64_t duration_us,
    int count) {
  if (count <= 0 || duration_us <= 0) {
    return {};
  }

  std::vector<std::int64_t> timestamps;
  timestamps.reserve(static_cast<std::size_t>(count));

  // Clamp the safe end to leave a margin before EOF.
  const std::int64_t safe_end =
      (duration_us > kEndMarginUs) ? (duration_us - kEndMarginUs) : duration_us;

  for (int i = 0; i < count; ++i) {
    // Midpoint of the i-th of N equal intervals within [0, safe_end].
    const std::int64_t ts = safe_end * (2 * i + 1) / (2 * count);
    timestamps.push_back(ts);
  }
  return timestamps;
}

}  // namespace

RealFrameSamplingResult build_real_frame_color_sampling_input(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path) {
  RealFrameSamplingResult result;

  // --- availability check ---
  if (!ffmpeg_is_available(ffmpeg_path)) {
    result.real_decoding_attempted = false;
    result.skipped_reason = "ffmpeg not found at: " + ffmpeg_path.string();
    result.input = build_foundation_color_staging_sample_input();
    return result;
  }

  // --- duration ---
  const media::VideoStreamProbe& stream = plan.primary_video_stream;
  std::int64_t duration_us = 0;

  if (stream.timing.duration_pts.has_value() &&
      media::is_valid(stream.timing.timebase) &&
      stream.timing.timebase.numerator > 0) {
    duration_us = media::pts_to_microseconds(*stream.timing.duration_pts,
                                             stream.timing.timebase);
  } else if (plan.probe.container_timing.has_value() &&
             plan.probe.container_timing->duration_pts.has_value() &&
             media::is_valid(plan.probe.container_timing->timebase) &&
             plan.probe.container_timing->timebase.numerator > 0) {
    duration_us =
        media::pts_to_microseconds(*plan.probe.container_timing->duration_pts,
                                   plan.probe.container_timing->timebase);
  }

  if (duration_us <= 0) {
    result.real_decoding_attempted = false;
    result.skipped_reason =
        "video stream duration unknown; cannot compute deterministic seek timestamps";
    result.input = build_foundation_color_staging_sample_input();
    return result;
  }

  // --- canonical raster ---
  const int width = plan.canonical_raster.width;
  const int height = plan.canonical_raster.height;
  if (width <= 0 || height <= 0) {
    result.real_decoding_attempted = false;
    result.skipped_reason = "canonical raster dimensions are not positive";
    result.input = build_foundation_color_staging_sample_input();
    return result;
  }

  // --- decode frames (with per-frame miss tolerance) ---
  result.real_decoding_attempted = true;

  const std::vector<std::int64_t> timestamps =
      deterministic_seek_timestamps_us(duration_us, kMaxDecodedFrames);

  result.frames_attempted = static_cast<int>(timestamps.size());

  std::vector<ColorRasterFrame> frames;
  std::vector<std::int64_t> decoded_timestamps;

  for (int i = 0; i < static_cast<int>(timestamps.size()); ++i) {
    std::string decode_error;
    std::vector<Srgb8Pixel> pixels = decode_frame_at(ffmpeg_path,
                                                      plan.source_path,
                                                      timestamps[i],
                                                      width,
                                                      height,
                                                      decode_error);
    if (pixels.empty()) {
      // Record the miss but continue; later timestamps may still decode.
      ++result.frames_missed;
      if (result.skipped_reason.empty()) {
        // Keep the first miss reason for the manifest.
        result.skipped_reason = "frame miss at " + std::to_string(timestamps[i]) +
                                "us: " + decode_error;
      }
      continue;
    }

    const bool is_keyframe = (decoded_timestamps.empty());
    frames.push_back(ColorRasterFrame{
        frame_id(static_cast<int>(decoded_timestamps.size())),
        timestamps[i],
        width,
        height,
        is_keyframe,
        std::move(pixels),
    });
    decoded_timestamps.push_back(timestamps[i]);
  }

  result.frames_decoded = static_cast<int>(frames.size());

  if (frames.empty()) {
    // Every targeted timestamp failed.
    result.real_decoding_succeeded = false;
    result.input = build_foundation_color_staging_sample_input();
    return result;
  }

  // --- build scenes and shots from the decoded frames ---
  // One scene spanning all decoded frames; one shot per adjacent frame pair
  // (or a single shot if only one frame decoded).
  std::vector<std::string> all_frame_ids;
  all_frame_ids.reserve(frames.size());
  for (const ColorRasterFrame& f : frames) {
    all_frame_ids.push_back(f.frame_id);
  }

  ColorTimelineRange full_scene;
  full_scene.target_id = "scene_000001";
  full_scene.start_us = decoded_timestamps.front();
  full_scene.end_us = decoded_timestamps.back();
  full_scene.frame_ids = all_frame_ids;

  std::vector<ColorTimelineRange> shots;
  if (frames.size() == 1) {
    ColorTimelineRange shot;
    shot.target_id = "shot_000001";
    shot.start_us = decoded_timestamps.front();
    shot.end_us = decoded_timestamps.front();
    shot.frame_ids = {frames[0].frame_id};
    shots.push_back(std::move(shot));
  } else {
    for (std::size_t i = 0; i + 1 < decoded_timestamps.size(); ++i) {
      ColorTimelineRange shot;
      std::ostringstream id_oss;
      id_oss << "shot_" << std::setw(6) << std::setfill('0') << (i + 1);
      shot.target_id = id_oss.str();
      shot.start_us = decoded_timestamps[i];
      shot.end_us = decoded_timestamps[i + 1];
      shot.frame_ids = {frames[i].frame_id, frames[i + 1].frame_id};
      shots.push_back(std::move(shot));
    }
  }

  result.real_decoding_succeeded = true;
  result.input = ColorFrameSamplingInput{
      std::move(frames),
      {std::move(full_scene)},
      std::move(shots),
  };
  return result;
}

}  // namespace svp::vision
