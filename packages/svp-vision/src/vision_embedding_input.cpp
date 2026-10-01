#include "svp/vision/vision_embedding_input.hpp"

#include <cmath>
#include <cstddef>

namespace svp::vision {

cv::Mat srgb8_frame_to_cv_mat(const ColorRasterFrame& frame) {
  cv::Mat mat(frame.height, frame.width, CV_8UC3);
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      const auto& px = frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
      mat.at<cv::Vec3b>(y, x) = cv::Vec3b(px.r, px.g, px.b);
    }
  }
  return mat;
}

std::vector<float> frame_to_clip_normalized_chw(
    const cv::Mat& resized_frame) {
  const int width = resized_frame.cols;
  const int height = resized_frame.rows;
  const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
  std::vector<float> output(pixel_count * 3);
  constexpr float kMean[3] = {0.48145466f, 0.4578275f, 0.40821073f};
  constexpr float kStd[3] = {0.26862954f, 0.26130258f, 0.27577711f};
  for (int i = 0; i < static_cast<int>(pixel_count); ++i) {
    const auto* px = resized_frame.ptr<cv::Vec3b>(i / width) + (i % width);
    output[i] = (static_cast<float>((*px)[0]) / 255.0f - kMean[0]) / kStd[0];
    output[pixel_count + i] = (static_cast<float>((*px)[1]) / 255.0f - kMean[1]) / kStd[1];
    output[2 * pixel_count + i] = (static_cast<float>((*px)[2]) / 255.0f - kMean[2]) / kStd[2];
  }
  return output;
}

void l2_normalize(std::vector<float>& vec) {
  double norm = 0;
  for (float v : vec) norm += v * v;
  norm = std::sqrt(norm);
  if (norm > 0) {
    for (float& v : vec) v = static_cast<float>(v / norm);
  }
}

}  // namespace svp::vision
