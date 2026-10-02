#include "svp/vision/tasks/track_window_calibration_clip.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>

namespace svp::vision::tasks {
namespace {

// Tiles of unrelated colours: textured enough for corner tracking, flow,
// and depth to do real work everywhere in the frame.
constexpr int kTileSize = 16;
// Moving objects: their size, how far each moves per frame, and their count.
// Each crosses a few hundred pixels over the clip, as a subject crossing the
// frame does, and stays inside the raster.
constexpr int kObjectWidth = 96;
constexpr int kObjectHeight = 72;
constexpr int kObjectStepPx = 12;
constexpr int kObjectCount = 3;

std::uint32_t mix(std::uint32_t seed, int x, int y) {
  std::uint32_t hash = seed * 2246822519U + static_cast<std::uint32_t>(y) * 7919U +
                       static_cast<std::uint32_t>(x) * 104729U;
  return hash * 2654435761U;
}

cv::Vec3b colour(std::uint32_t hash) {
  return cv::Vec3b(static_cast<std::uint8_t>(hash >> 24), static_cast<std::uint8_t>(hash >> 16),
                   static_cast<std::uint8_t>(hash >> 8));
}

cv::Mat render_frame(std::size_t index) {
  cv::Mat frame(kTrackWindowCalibrationFrameHeight, kTrackWindowCalibrationFrameWidth, CV_8UC3);
  for (int y = 0; y < frame.rows; ++y) {
    for (int x = 0; x < frame.cols; ++x) {
      frame.at<cv::Vec3b>(y, x) = colour(mix(1, x / kTileSize, y / kTileSize));
    }
  }
  const int travel = frame.cols - kObjectWidth;
  for (int object = 0; object < kObjectCount; ++object) {
    const int lane_height = frame.rows / kObjectCount;
    const int top = object * lane_height + (lane_height - kObjectHeight) / 2;
    const int start = (object * travel) / kObjectCount;
    const int offset = static_cast<int>(index) * kObjectStepPx * (object % 2 == 0 ? 1 : -1);
    const int left = ((start + offset) % travel + travel) % travel;
    for (int y = 0; y < kObjectHeight; ++y) {
      for (int x = 0; x < kObjectWidth; ++x) {
        frame.at<cv::Vec3b>(top + y, left + x) =
            colour(mix(static_cast<std::uint32_t>(object) + 2, x / (kTileSize / 2),
                       y / (kTileSize / 2)));
      }
    }
  }
  return frame;
}

std::string shell_quoted(const std::string& text) {
  std::string quoted = "'";
  for (const char character : text) {
    quoted += character == '\'' ? std::string("'\\''") : std::string(1, character);
  }
  return quoted + "'";
}

}  // namespace

std::vector<std::int64_t> track_window_calibration_timestamps_us(std::int64_t sample_interval_us) {
  if (sample_interval_us <= 0) {
    throw std::invalid_argument("a tracking calibration needs a positive sample interval");
  }
  std::vector<std::int64_t> timestamps;
  for (std::size_t frame = 0; frame < kTrackWindowCalibrationFrames; ++frame) {
    timestamps.push_back(static_cast<std::int64_t>(frame) * sample_interval_us);
  }
  return timestamps;
}

std::filesystem::path write_track_window_calibration_clip(const std::filesystem::path& ffmpeg,
                                                          const std::filesystem::path& directory,
                                                          std::int64_t sample_interval_us) {
  const std::vector<std::int64_t> timestamps =
      track_window_calibration_timestamps_us(sample_interval_us);
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "track-window-calibration.mkv";
  // One frame per sample interval: -r 1000000/<interval> frames per second.
  const std::string command =
      shell_quoted(ffmpeg.string()) + " -v error -nostdin -y -f rawvideo -pix_fmt bgr24 -s " +
      std::to_string(kTrackWindowCalibrationFrameWidth) + "x" +
      std::to_string(kTrackWindowCalibrationFrameHeight) + " -r 1000000/" +
      std::to_string(sample_interval_us) +
      " -i - -an -c:v ffv1 -g 1 -fflags +bitexact -flags:v +bitexact " +
      shell_quoted(path.string());
  FILE* pipe = ::popen(command.c_str(), "w");
  if (pipe == nullptr) {
    throw std::runtime_error("cannot run " + ffmpeg.string() + " for the calibration clip");
  }
#ifdef F_SETNOSIGPIPE
  // If ffmpeg exits early, a write fails (EPIPE) and is reported below
  // instead of SIGPIPE ending the coordinator.
  ::fcntl(::fileno(pipe), F_SETNOSIGPIPE, 1);
#endif
  std::size_t written = 0;
  for (std::size_t index = 0; index < timestamps.size(); ++index) {
    const cv::Mat frame = render_frame(index);
    const std::size_t bytes = frame.total() * frame.elemSize();
    if (std::fwrite(frame.data, 1, bytes, pipe) != bytes) {
      break;
    }
    ++written;
  }
  const int status = ::pclose(pipe);
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      written != timestamps.size()) {
    throw std::runtime_error("ffmpeg could not encode the tracking calibration clip");
  }
  return path;
}

}  // namespace svp::vision::tasks
