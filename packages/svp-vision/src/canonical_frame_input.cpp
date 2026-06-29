#include "svp/vision/canonical_frame_input.hpp"

#include "svp/media/canonical_timing.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <sys/wait.h>

namespace svp::vision {
namespace {

// Maximum number of canonical frames to decode per build run.
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

std::string microseconds_to_seek_string(std::int64_t us) {
  const std::int64_t seconds = us / 1000000;
  const std::int64_t fraction = us % 1000000;
  std::ostringstream oss;
  oss << seconds << "." << std::setw(6) << std::setfill('0') << fraction;
  return oss.str();
}

std::string frame_id(int index) {
  std::ostringstream oss;
  oss << "frame_" << std::setw(6) << std::setfill('0') << (index + 1);
  return oss.str();
}

std::vector<Srgb8Pixel> decode_frame_at(const std::filesystem::path& ffmpeg_path,
                                         const std::filesystem::path& source_path,
                                         std::int64_t seek_us,
                                         int width,
                                         int height,
                                         std::string& out_error) {
  const std::string seek = microseconds_to_seek_string(seek_us);

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

std::vector<std::int64_t> deterministic_seek_timestamps_us(
    std::int64_t duration_us,
    int count) {
  if (count <= 0 || duration_us <= 0) {
    return {};
  }

  std::vector<std::int64_t> timestamps;
  timestamps.reserve(static_cast<std::size_t>(count));

  const std::int64_t safe_end =
      (duration_us > kEndMarginUs) ? (duration_us - kEndMarginUs) : duration_us;

  for (int i = 0; i < count; ++i) {
    const std::int64_t ts = safe_end * (2 * i + 1) / (2 * count);
    timestamps.push_back(ts);
  }
  return timestamps;
}

}  // namespace

std::int64_t compute_media_duration_us(const media::MediaIngestPlan& plan) {
  const media::VideoStreamProbe& stream = plan.primary_video_stream;
  if (stream.timing.duration_pts.has_value() &&
      media::is_valid(stream.timing.timebase) &&
      stream.timing.timebase.numerator > 0) {
    return media::pts_to_microseconds(*stream.timing.duration_pts,
                                      stream.timing.timebase);
  }
  if (plan.probe.container_timing.has_value() &&
      plan.probe.container_timing->duration_pts.has_value() &&
      media::is_valid(plan.probe.container_timing->timebase) &&
      plan.probe.container_timing->timebase.numerator > 0) {
    return media::pts_to_microseconds(*plan.probe.container_timing->duration_pts,
                                      plan.probe.container_timing->timebase);
  }
  return 0;
}

DecodedCanonicalFrames decode_canonical_frames(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path,
    FrameCatalog* frame_catalog) {
  return decode_frames_at_resolution(
      plan, ffmpeg_path,
      plan.canonical_raster.width,
      plan.canonical_raster.height,
      kMaxDecodedFrames,
      frame_catalog,
      "canonical");
}

DecodedCanonicalFrames decode_frames_at_resolution(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path,
    int target_width,
    int target_height,
    int max_frames,
    FrameCatalog* frame_catalog,
    const std::string& purpose) {
  DecodedCanonicalFrames result;

  if (!ffmpeg_is_available(ffmpeg_path)) {
    result.decoding_attempted = false;
    result.skipped_reason = "ffmpeg not found at: " + ffmpeg_path.string();
    return result;
  }

  const std::int64_t duration_us = compute_media_duration_us(plan);

  if (duration_us <= 0) {
    result.decoding_attempted = false;
    result.skipped_reason =
        "video stream duration unknown; cannot compute deterministic seek timestamps";
    return result;
  }

  const int width = target_width;
  const int height = target_height;
  if (width <= 0 || height <= 0) {
    result.decoding_attempted = false;
    result.skipped_reason = "target frame dimensions are not positive";
    return result;
  }

  result.decoding_attempted = true;

  const int frame_count = max_frames > 0 ? max_frames : kMaxDecodedFrames;
  const std::vector<std::int64_t> timestamps =
      deterministic_seek_timestamps_us(duration_us, frame_count);

  return decode_frames_at_timestamps(plan, ffmpeg_path, width, height, timestamps,
                                      frame_catalog, purpose);
}

DecodedCanonicalFrames decode_frames_at_timestamps(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path,
    int target_width,
    int target_height,
    const std::vector<std::int64_t>& timestamps_us,
    FrameCatalog* frame_catalog,
    const std::string& purpose) {
  DecodedCanonicalFrames result;

  if (!ffmpeg_is_available(ffmpeg_path)) {
    result.decoding_attempted = false;
    result.skipped_reason = "ffmpeg not found at: " + ffmpeg_path.string();
    return result;
  }

  const int width = target_width;
  const int height = target_height;
  if (width <= 0 || height <= 0) {
    result.decoding_attempted = false;
    result.skipped_reason = "target frame dimensions are not positive";
    return result;
  }

  result.decoding_attempted = true;
  result.frames_attempted = static_cast<int>(timestamps_us.size());

  for (int i = 0; i < static_cast<int>(timestamps_us.size()); ++i) {
    std::string decode_error;
    std::vector<Srgb8Pixel> pixels = decode_frame_at(ffmpeg_path,
                                                      plan.source_path,
                                                      timestamps_us[static_cast<std::size_t>(i)],
                                                      width,
                                                      height,
                                                      decode_error);
    if (pixels.empty()) {
      ++result.frames_missed;
      if (result.skipped_reason.empty()) {
        result.skipped_reason = "frame miss at " + std::to_string(timestamps_us[static_cast<std::size_t>(i)]) +
                                "us: " + decode_error;
      }
      continue;
    }

    const bool is_keyframe = result.frames.empty();
    std::string fid;
    std::size_t fidx = result.frames.size();
    if (frame_catalog) {
      fid = frame_catalog->register_frame(
          timestamps_us[static_cast<std::size_t>(i)], width, height,
          purpose, is_keyframe);
      const auto idx = frame_catalog->get_frame_index(
          timestamps_us[static_cast<std::size_t>(i)], width, height);
      if (idx.has_value())
        fidx = *idx;
    } else {
      fid = frame_id(static_cast<int>(result.frames.size()));
    }
    ColorRasterFrame frame{
        fid,
        timestamps_us[static_cast<std::size_t>(i)],
        width,
        height,
        is_keyframe,
        std::move(pixels),
    };
    frame.frame_index = fidx;
    result.frames.push_back(std::move(frame));
  }

  result.frames_decoded = static_cast<int>(result.frames.size());

  if (result.frames.empty()) {
    result.decoding_succeeded = false;
    return result;
  }

  result.decoding_succeeded = true;
  return result;
}

DecodedCanonicalFrames decode_frames_at_timestamps_streaming(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path,
    int target_width,
    int target_height,
    const std::vector<std::int64_t>& timestamps_us,
    const std::function<void(const ColorRasterFrame&, std::size_t)>& on_frame,
    FrameCatalog* frame_catalog,
    const std::string& purpose) {
  DecodedCanonicalFrames result;

  if (!ffmpeg_is_available(ffmpeg_path)) {
    result.decoding_attempted = false;
    result.skipped_reason = "ffmpeg not found at: " + ffmpeg_path.string();
    return result;
  }

  const int width = target_width;
  const int height = target_height;
  if (width <= 0 || height <= 0) {
    result.decoding_attempted = false;
    result.skipped_reason = "target frame dimensions are not positive";
    return result;
  }

  result.decoding_attempted = true;
  result.frames_attempted = static_cast<int>(timestamps_us.size());

  for (int i = 0; i < static_cast<int>(timestamps_us.size()); ++i) {
    std::string decode_error;
    std::vector<Srgb8Pixel> pixels = decode_frame_at(
        ffmpeg_path,
        plan.source_path,
        timestamps_us[static_cast<std::size_t>(i)],
        width,
        height,
        decode_error);
    if (pixels.empty()) {
      ++result.frames_missed;
      if (result.skipped_reason.empty()) {
        result.skipped_reason =
            "frame miss at " +
            std::to_string(timestamps_us[static_cast<std::size_t>(i)]) +
            "us: " + decode_error;
      }
      continue;
    }

    std::string fid;
    std::size_t fidx = static_cast<std::size_t>(result.frames_decoded);
    if (frame_catalog) {
      fid = frame_catalog->register_frame(
          timestamps_us[static_cast<std::size_t>(i)], width, height,
          purpose, result.frames_decoded == 0);
      const auto idx = frame_catalog->get_frame_index(
          timestamps_us[static_cast<std::size_t>(i)], width, height);
      if (idx.has_value())
        fidx = *idx;
    } else {
      fid = frame_id(result.frames_decoded);
    }
    ColorRasterFrame frame{
        fid,
        timestamps_us[static_cast<std::size_t>(i)],
        width,
        height,
        result.frames_decoded == 0,
        std::move(pixels),
    };
    frame.frame_index = fidx;
    on_frame(frame, fidx);
    ++result.frames_decoded;
  }

  result.decoding_succeeded = result.frames_decoded > 0;
  return result;
}

}  // namespace svp::vision
