#include "svp/vision/evidence_crop_work.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <sys/wait.h>

namespace svp::vision {
namespace {

std::string shell_quote(const std::string& text) {
  std::string quoted = "'";
  for (const char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

std::string shell_quote(const std::filesystem::path& path) {
  return shell_quote(path.string());
}

std::string trim(const std::string& s) {
  std::size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string microseconds_to_seek_string(std::int64_t us) {
  const std::int64_t seconds = us / 1000000;
  const std::int64_t fraction = us % 1000000;
  std::ostringstream oss;
  oss << seconds << "." << std::setw(6) << std::setfill('0') << fraction;
  return oss.str();
}

// Crops narrower than kMinOcrCropWidth are upscaled 2x (at most to
// kMaxCropWidth) so the recognizer sees readable glyphs; wider ones are
// scaled down to kMaxCropWidth. The evidence-crop stage has always encoded
// crops this way; the values are part of the crop bytes every build writes.
constexpr int kMinOcrCropWidth = 500;
constexpr int kMaxCropWidth = 2000;

struct ScaledCropSize {
  int width = 0;
  int height = 0;
  bool scaled = false;
};

ScaledCropSize scaled_crop_size(int target_width, int target_height) {
  if (target_width < kMinOcrCropWidth) {
    const int scaled_width = std::min(target_width * 2, kMaxCropWidth);
    const int scaled_height = static_cast<int>(
        std::round(static_cast<double>(target_height) * scaled_width /
                   std::max(1, target_width)));
    return {scaled_width, scaled_height, true};
  }
  if (target_width > kMaxCropWidth) {
    const int scaled_width = kMaxCropWidth;
    const int scaled_height = static_cast<int>(
        std::round(static_cast<double>(target_height) * scaled_width /
                   std::max(1, target_width)));
    return {scaled_width, scaled_height, true};
  }
  return {target_width, target_height, false};
}

}  // namespace

std::uint64_t evidence_crop_image_pixels(const EvidenceCropJob& job) {
  const ScaledCropSize size = scaled_crop_size(job.width, job.height);
  return static_cast<std::uint64_t>(std::max(0, size.width)) *
         static_cast<std::uint64_t>(std::max(0, size.height));
}

bool extract_evidence_crop_image(const std::filesystem::path& ffmpeg_path,
                                 const std::filesystem::path& source_path,
                                 const EvidenceCropJob& job,
                                 const std::filesystem::path& output_path,
                                 std::string& error) {
  const std::string seek = microseconds_to_seek_string(job.seek_us);
  const int target_width = job.width;
  const int target_height = job.height;

  std::string crop_filter =
      "crop=" + std::to_string(job.width) + ":" +
      std::to_string(job.height) + ":" +
      std::to_string(job.left) + ":" +
      std::to_string(job.top);

  if (const ScaledCropSize scaled = scaled_crop_size(target_width, target_height);
      scaled.scaled) {
    crop_filter += ",scale=" + std::to_string(scaled.width) + ":" +
                    std::to_string(scaled.height);
  }

  std::string codec_opts;
  if (job.image_format == "jpeg" || job.image_format == "jpg") {
    codec_opts = " -c:v mjpeg -q:v " + std::to_string(
        std::max(1, std::min(31, 31 - (job.jpeg_quality * 31) / 100)));
    crop_filter += ",format=yuvj420p";
  }

  std::string cmd =
      shell_quote(ffmpeg_path) +
      " -v error"
      " -ss " + seek +
      " -i " + shell_quote(source_path) +
      " -vf " + shell_quote(crop_filter) +
      " -vframes 1" +
      codec_opts +
      " -y " + shell_quote(output_path) +
      " 2>&1";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    error = "popen failed: " + std::string(std::strerror(errno));
    return false;
  }

  std::string output;
  char buffer[4096];
  while (true) {
    const std::size_t n = fread(buffer, 1, sizeof(buffer), pipe);
    if (n == 0) break;
    output.append(buffer, n);
  }
  const int status = pclose(pipe);
  const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
      std::filesystem::exists(output_path);

  if (!ok) {
    error = trim(output);
    if (error.empty()) {
      error = "ffmpeg exited with status " +
              std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status);
    }
  }
  return ok;
}

std::optional<ColorRasterFrame> decode_evidence_crop_image(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& image_path,
    int width,
    int height,
    const std::string& frame_id,
    std::int64_t timestamp_us) {
  if (width <= 0 || height <= 0 || !std::filesystem::exists(image_path)) {
    return std::nullopt;
  }

  const std::string cmd =
      shell_quote(ffmpeg_path) +
      " -v error"
      " -i " + shell_quote(image_path) +
      " -vf scale=" + std::to_string(width) + ":" + std::to_string(height) +
      " -vframes 1"
      " -f rawvideo"
      " -pix_fmt rgb24"
      " pipe:1"
      " 2>/dev/null";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr) return std::nullopt;

  const std::size_t expected_bytes =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
  std::vector<std::uint8_t> raw_bytes(expected_bytes);
  const std::size_t bytes_read = std::fread(raw_bytes.data(), 1, expected_bytes, pipe);
  const int status = pclose(pipe);

  if (bytes_read != expected_bytes ||
      !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }

  ColorRasterFrame frame;
  frame.frame_id = frame_id;
  frame.frame_index = 0;
  frame.timestamp_us = timestamp_us;
  frame.width = width;
  frame.height = height;
  frame.keyframe = false;
  frame.pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for (std::size_t i = 0; i < raw_bytes.size(); i += 3) {
    frame.pixels.push_back({raw_bytes[i], raw_bytes[i + 1], raw_bytes[i + 2]});
  }
  return frame;
}

EvidenceCropRoi reread_evidence_crop(const PpOcrSession& session,
                                     const PpOcrOptions& options,
                                     const std::optional<ColorRasterFrame>& crop) {
  EvidenceCropRoi roi;
  if (!crop.has_value()) {
    return roi;
  }
  roi.decoded = true;
  const PpOcrDetection detection = run_pp_ocr_recognition_on_crop(session, options, *crop);
  roi.text = detection.text;
  roi.score = detection.score;
  return roi;
}

EvidenceCropJobOutcome run_evidence_crop_job(const std::filesystem::path& ffmpeg_path,
                                             const std::filesystem::path& source_path,
                                             const PpOcrSession& session,
                                             const PpOcrOptions& options,
                                             const EvidenceCropJob& job,
                                             const std::filesystem::path& scratch_path) {
  EvidenceCropJobOutcome outcome;
  outcome.ordinal = job.ordinal;
  struct RemoveScratch {
    const std::filesystem::path& path;
    ~RemoveScratch() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } remove_scratch{scratch_path};

  std::string error;
  if (!extract_evidence_crop_image(ffmpeg_path, source_path, job, scratch_path, error)) {
    return outcome;
  }
  std::ifstream file(scratch_path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
  if (!file && !file.eof()) {
    return outcome;
  }
  outcome.extracted = true;
  outcome.image.resize(bytes.size());
  std::memcpy(outcome.image.data(), bytes.data(), bytes.size());
  outcome.roi = reread_evidence_crop(
      session, options,
      decode_evidence_crop_image(ffmpeg_path, scratch_path, job.width, job.height, "",
                                 job.seek_us));
  return outcome;
}

}  // namespace svp::vision
