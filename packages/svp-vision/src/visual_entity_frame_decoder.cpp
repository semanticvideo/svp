#include "svp/vision/visual_entity_frame_decoder.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace svp::vision {
namespace {

std::string shell_quote(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::string quoted = "\"";
  for (const char c : path.string()) {
    quoted += c == '\"' ? "\\\"" : std::string(1, c);
  }
  return quoted + "\"";
#else
  std::string quoted = "'";
  for (const char c : path.string()) {
    quoted += c == '\'' ? "'\\''" : std::string(1, c);
  }
  return quoted + "'";
#endif
}

std::string seconds(std::int64_t microseconds) {
  std::ostringstream value;
  value << microseconds / 1000000 << '.' << std::setw(6) << std::setfill('0')
        << microseconds % 1000000;
  return value.str();
}

FILE* open_pipe(const char* command) {
#if defined(_WIN32)
  return _popen(command, "rb");
#else
  return popen(command, "r");
#endif
}

bool close_pipe(FILE* pipe) {
#if defined(_WIN32)
  return _pclose(pipe) == 0;
#else
  const int status = pclose(pipe);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

std::string fallback_frame_id(std::size_t index) {
  std::ostringstream value;
  value << "frame_" << std::setw(6) << std::setfill('0') << index + 1;
  return value.str();
}

}  // namespace

DecodedCanonicalFrames decode_visual_entity_window(
    const media::MediaIngestPlan& media_plan,
    const std::filesystem::path& ffmpeg_path,
    int width,
    int height,
    const std::vector<std::int64_t>& timestamps_us,
    FrameCatalog* frame_catalog,
    const std::string& purpose) {
  DecodedCanonicalFrames result;
  if (timestamps_us.empty()) return result;
  if (width <= 0 || height <= 0) {
    result.skipped_reason = "visual entity frame dimensions are not positive";
    return result;
  }
  if (timestamps_us.size() > 1) {
    const auto interval_us = timestamps_us[1] - timestamps_us[0];
    const bool uniform = interval_us > 0 &&
        std::adjacent_find(
            timestamps_us.begin() + 1, timestamps_us.end(),
            [interval_us](const auto left, const auto right) {
              return right - left != interval_us;
            }) == timestamps_us.end();
    if (!uniform) {
      result.skipped_reason =
          "visual entity window timestamps must use a uniform cadence";
      return result;
    }
  }
  if (ffmpeg_path.has_parent_path() && !std::filesystem::exists(ffmpeg_path)) {
    result.skipped_reason = "ffmpeg not found at: " + ffmpeg_path.string();
    return result;
  }

  result.decoding_attempted = true;
  result.frames_attempted = static_cast<int>(timestamps_us.size());
  const std::int64_t start_us = timestamps_us.front();
  const std::int64_t duration_us =
      timestamps_us.size() > 1
          ? timestamps_us.back() - start_us +
                (timestamps_us[1] - timestamps_us[0])
          : 1;
  const double frames_per_second = timestamps_us.size() > 1
      ? 1000000.0 /
            static_cast<double>(timestamps_us[1] - timestamps_us[0])
      : 1.0;

  std::ostringstream command;
  command << shell_quote(ffmpeg_path) << " -v error -ss " << seconds(start_us)
          << " -i " << shell_quote(media_plan.source_path)
          << " -t " << seconds(duration_us)
          << " -vf fps=" << std::setprecision(12) << frames_per_second
          << ",scale=" << width << ':' << height
          << " -frames:v " << timestamps_us.size()
          << " -f rawvideo -pix_fmt rgb24 pipe:1";
#if defined(_WIN32)
  command << " 2>NUL";
#else
  command << " 2>/dev/null";
#endif

  FILE* pipe = open_pipe(command.str().c_str());
  if (pipe == nullptr) {
    result.skipped_reason =
        std::string("failed to start ffmpeg: ") + std::strerror(errno);
    return result;
  }

  const std::size_t frame_bytes =
      static_cast<std::size_t>(width) * height * 3;
  std::vector<std::uint8_t> raw(frame_bytes);
  for (std::size_t index = 0; index < timestamps_us.size(); ++index) {
    const std::size_t bytes_read = std::fread(raw.data(), 1, frame_bytes, pipe);
    if (bytes_read != frame_bytes) {
      result.frames_missed +=
          static_cast<int>(timestamps_us.size() - index);
      break;
    }

    std::vector<Srgb8Pixel> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height);
    for (std::size_t byte = 0; byte < raw.size(); byte += 3) {
      pixels.push_back({raw[byte], raw[byte + 1], raw[byte + 2]});
    }

    std::string id;
    std::size_t frame_index = index;
    if (frame_catalog != nullptr) {
      id = frame_catalog->register_frame(
          timestamps_us[index], purpose, index == 0);
      if (const auto registered =
              frame_catalog->get_frame_index(timestamps_us[index])) {
        frame_index = *registered;
      }
    } else {
      id = fallback_frame_id(index);
    }
    ColorRasterFrame frame{
        id, timestamps_us[index], width, height, index == 0,
        std::move(pixels)};
    frame.frame_index = frame_index;
    result.frames.push_back(std::move(frame));
  }

  const bool exited_cleanly = close_pipe(pipe);
  result.frames_decoded = static_cast<int>(result.frames.size());
  result.decoding_succeeded = exited_cleanly && !result.frames.empty();
  if (!exited_cleanly && result.skipped_reason.empty()) {
    result.skipped_reason = "ffmpeg window decoder exited unsuccessfully";
  }
  return result;
}

}  // namespace svp::vision
