#include "pp_ocr_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace svp::vision::pp_ocr_internal {
namespace {

// PP-OCRv6 is exported around a 3x48x320 recognition canvas while permitting
// wider dynamic inputs for unusually long text. Keep ordinary crops at the
// model's native width and grow in convolution-friendly blocks only as needed.
constexpr int kRecognitionBaseWidth = 320;
constexpr int kRecognitionWidthAlignment = 32;

int aligned_recognition_width(int resized_width, int max_width) {
  if (max_width < kRecognitionBaseWidth ||
      max_width % kRecognitionWidthAlignment != 0) {
    throw std::invalid_argument(
        "PP-OCR recognition max width must be at least 320 pixels and "
        "aligned to 32 pixels");
  }
  const int aligned =
      ((resized_width + kRecognitionWidthAlignment - 1) /
       kRecognitionWidthAlignment) * kRecognitionWidthAlignment;
  return std::clamp(aligned, kRecognitionBaseWidth, max_width);
}

}  // namespace

RecInput preprocess_recognition(
    const ColorRasterFrame& frame,
    const DetBox& box,
    int target_height,
    int max_width) {
  int x1 = std::max(0, box.left);
  int y1 = std::max(0, box.top);
  int x2 = std::min(frame.width, box.right);
  int y2 = std::min(frame.height, box.bottom);
  if (x2 <= x1 || y2 <= y1) return {};

  const int crop_w = x2 - x1;
  const int crop_h = y2 - y1;

  double scale = static_cast<double>(target_height) / crop_h;
  int resized_w = std::min(static_cast<int>(crop_w * scale), max_width);
  if (resized_w < 1) resized_w = 1;
  const int input_width = aligned_recognition_width(resized_w, max_width);

  RecInput input;
  input.height = target_height;
  input.width = input_width;
  input.data.resize(
      static_cast<std::size_t>(3) * target_height * input_width, 0.0f);

  for (int c = 0; c < 3; ++c) {
    const int src_c = (c == 0) ? 2 : (c == 1) ? 1 : 0;
    for (int y = 0; y < target_height; ++y) {
      const int src_y = std::min(static_cast<int>(y / scale), crop_h - 1);
      for (int x = 0; x < resized_w; ++x) {
        const int src_x = std::min(static_cast<int>(x / scale), crop_w - 1);
        const auto& px = frame.pixels[(y1 + src_y) * frame.width + x1 + src_x];
        const uint8_t val = (src_c == 0) ? px.r : (src_c == 1) ? px.g : px.b;
        input.data[c * target_height * input_width + y * input_width + x] =
            static_cast<float>(val) / 255.0f;
      }
    }
  }

  return input;
}

std::string ctc_decode(
    const float* pred_data,
    int timesteps,
    int num_classes,
    const std::vector<std::string>& char_dict) {
  std::string result;
  int prev_idx = 0;
  for (int t = 0; t < timesteps; ++t) {
    int max_idx = 0;
    float max_val = pred_data[t * num_classes];
    for (int c = 1; c < num_classes; ++c) {
      const float v = pred_data[t * num_classes + c];
      if (v > max_val) {
        max_val = v;
        max_idx = c;
      }
    }
    if (max_idx != 0 && max_idx != prev_idx) {
      if (max_idx - 1 < static_cast<int>(char_dict.size())) {
        result += char_dict[max_idx - 1];
      }
    }
    prev_idx = max_idx;
  }
  return result;
}

double recognition_confidence(
    const float* pred_data,
    int timesteps,
    int num_classes) {
  double conf = 0.0;
  int non_blank_count = 0;
  for (int t = 0; t < timesteps; ++t) {
    const float* row = pred_data + t * num_classes;
    int max_idx = 0;
    float max_val = row[0];
    for (int c = 1; c < num_classes; ++c) {
      if (row[c] > max_val) {
        max_val = row[c];
        max_idx = c;
      }
    }
    if (max_idx != 0) {
      float sum_exp = 0.0f;
      for (int c = 0; c < num_classes; ++c) {
        sum_exp += std::exp(row[c] - max_val);
      }
      conf += 1.0f / sum_exp;
      ++non_blank_count;
    }
  }
  if (non_blank_count > 0) conf /= non_blank_count;
  return conf;
}

}  // namespace svp::vision::pp_ocr_internal
