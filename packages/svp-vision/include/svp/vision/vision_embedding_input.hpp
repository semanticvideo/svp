#pragma once

#include "svp/vision/color_frame_sampling.hpp"

#include <opencv2/core.hpp>

#include <vector>

namespace svp::vision {

// Input side length of Nomic Embed Vision v1.5 (CLIP-style ViT): images are
// resized to this square before inference.
inline constexpr int kVisionEmbeddingInputSide = 224;

// Decoded sRGB8 raster frame as an OpenCV RGB matrix.
[[nodiscard]] cv::Mat srgb8_frame_to_cv_mat(const ColorRasterFrame& frame);

// CLIP-style preprocessing for Nomic Embed Vision: rescale to [0,1] and
// normalize with CLIP mean/std, CHW float layout. The input must already be
// resized to kVisionEmbeddingInputSide x kVisionEmbeddingInputSide.
[[nodiscard]] std::vector<float> frame_to_clip_normalized_chw(
    const cv::Mat& resized_frame);

// In-place L2 normalization; leaves an all-zero vector unchanged.
void l2_normalize(std::vector<float>& vec);

}  // namespace svp::vision
