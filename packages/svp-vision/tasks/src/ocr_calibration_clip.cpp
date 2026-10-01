#include "svp/vision/tasks/ocr_calibration_clip.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <array>
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/wait.h>

namespace svp::vision::tasks {
namespace {

// Text layout: a grid of cells, each holding at most one line, so lines
// never touch and the detector sees one box per line. 4 columns x 32 rows
// of 480 x 33 px fill the frame and hold the heaviest frame's 128 lines.
constexpr int kColumns = 4;
constexpr int kRows = 32;
constexpr int kCellWidth = kOcrCalibrationFrameWidth / kColumns;
constexpr int kCellHeight = kOcrCalibrationFrameHeight / kRows;
// Hershey simplex at this scale is about 15 px per character and 17 px
// tall: a readable line of kLineCharacters that leaves a margin in its cell.
constexpr double kFontScale = 0.8;
constexpr int kFontThickness = 2;
constexpr int kLineCharacters = 12;
constexpr int kCellMargin = 8;

// A fixed linear congruential sequence (Numerical Recipes constants): the
// clip is the same on every Mac and every run.
class Sequence {
 public:
  explicit Sequence(std::uint32_t seed) : state_(seed) {}
  std::uint32_t next() {
    state_ = state_ * 1664525U + 1013904223U;
    return state_ >> 8;
  }

 private:
  std::uint32_t state_;
};

cv::Mat render_frame(int lines, std::uint32_t seed) {
  cv::Mat frame(kOcrCalibrationFrameHeight, kOcrCalibrationFrameWidth, CV_8UC3,
                cv::Scalar(255, 255, 255));
  Sequence sequence(seed);
  std::array<int, kColumns * kRows> cells{};
  std::iota(cells.begin(), cells.end(), 0);
  for (std::size_t index = cells.size() - 1; index > 0; --index) {
    std::swap(cells[index], cells[sequence.next() % (index + 1)]);
  }
  static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789";
  for (int line = 0; line < lines; ++line) {
    std::string text;
    for (int character = 0; character < kLineCharacters; ++character) {
      text += kAlphabet[sequence.next() % (sizeof(kAlphabet) - 1)];
    }
    const int cell = cells[static_cast<std::size_t>(line)];
    const cv::Point origin((cell % kColumns) * kCellWidth + kCellMargin,
                           (cell / kColumns + 1) * kCellHeight - kCellMargin);
    cv::putText(frame, text, origin, cv::FONT_HERSHEY_SIMPLEX, kFontScale, cv::Scalar(0, 0, 0),
                kFontThickness, cv::LINE_AA);
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

std::vector<std::int64_t> ocr_calibration_timestamps_us() {
  std::vector<std::int64_t> timestamps;
  for (std::size_t frame = 0; frame < std::size(kOcrCalibrationFrameLines); ++frame) {
    timestamps.push_back(static_cast<std::int64_t>(frame) * kOcrCalibrationFrameIntervalUs);
  }
  return timestamps;
}

OcrCalibrationClip write_ocr_calibration_clip(const std::filesystem::path& ffmpeg,
                                              const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  OcrCalibrationClip clip;
  clip.path = directory / "ocr-calibration.mkv";
  const std::string command =
      shell_quoted(ffmpeg.string()) +
      " -v error -nostdin -y -f rawvideo -pix_fmt bgr24 -s " +
      std::to_string(kOcrCalibrationFrameWidth) + "x" +
      std::to_string(kOcrCalibrationFrameHeight) +
      " -r 1 -i - -an -c:v ffv1 -g 1 -fflags +bitexact -flags:v +bitexact " +
      shell_quoted(clip.path.string());
  FILE* pipe = ::popen(command.c_str(), "w");
  if (pipe == nullptr) {
    throw std::runtime_error("cannot run " + ffmpeg.string() + " for the calibration clip");
  }
  std::int64_t timestamp = 0;
  std::uint32_t seed = 1;
  for (const int lines : kOcrCalibrationFrameLines) {
    const cv::Mat frame = render_frame(lines, seed++);
    const std::size_t bytes = frame.total() * frame.elemSize();
    if (std::fwrite(frame.data, 1, bytes, pipe) != bytes) {
      break;
    }
    clip.timestamps_us.push_back(timestamp);
    timestamp += kOcrCalibrationFrameIntervalUs;
  }
  const int status = ::pclose(pipe);
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      clip.timestamps_us.size() != std::size(kOcrCalibrationFrameLines)) {
    throw std::runtime_error("ffmpeg could not encode the OCR calibration clip");
  }
  return clip;
}

}  // namespace svp::vision::tasks
