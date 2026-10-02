#pragma once

// The fixed calibration slice OCR capacity is measured on (plan §3.5: each
// Mac measures ocr.frame_batch throughput at increasing slot counts). It is a
// short synthetic clip of printed text that every Mac decodes and reads the
// same way, so measurements of different Macs compare like for like.
//
// Content: kOcrCalibrationFrameLines lines of black text on white, one frame
// per second at 1920x1080 (the OCR decode bound, kOcrMaxFrameDimension).
// OCR cost grows with the number of text boxes, and the recognizer runs its
// worker threads only from PpOcrOptions::recognition_parallel_min_boxes boxes
// up, so the frames span light to heavy pages on both sides of that bound: their mean (44 lines) is close to the
// reference fixture's measured mean of 48 boxes per frame (plan §2.2: 28,899
// boxes over 602 frames), from its median (4) to near its maximum (153).
// Frames are lossless FFV1, every frame a key frame, so a seek lands on
// exactly the planned frame.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace svp::vision::tasks {

// Bumped whenever the clip's content or encoding changes; calibration
// records name it so a changed slice invalidates old measurements.
inline constexpr std::uint32_t kOcrCalibrationRecipeVersion = 2;

inline constexpr int kOcrCalibrationFrameWidth = 1920;
inline constexpr int kOcrCalibrationFrameHeight = 1080;
inline constexpr std::int64_t kOcrCalibrationFrameIntervalUs = 1'000'000;
inline constexpr int kOcrCalibrationFrameLines[] = {4, 16, 48, 112};

// The clip's frame timestamps, known without writing it.
[[nodiscard]] std::vector<std::int64_t> ocr_calibration_timestamps_us();

struct OcrCalibrationClip {
  std::filesystem::path path;
  // One per frame, in order.
  std::vector<std::int64_t> timestamps_us;
};

// Renders the clip with OpenCV and encodes it with `ffmpeg` into
// `directory/ocr-calibration.mkv`. Throws std::runtime_error when ffmpeg
// fails.
[[nodiscard]] OcrCalibrationClip write_ocr_calibration_clip(
    const std::filesystem::path& ffmpeg, const std::filesystem::path& directory);

}  // namespace svp::vision::tasks
