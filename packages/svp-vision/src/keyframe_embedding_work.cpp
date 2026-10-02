#include "svp/vision/keyframe_embedding_work.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/vision_embedding_input.hpp"

#include <opencv2/imgproc.hpp>

namespace svp::vision {

std::vector<float> keyframe_model_input(const std::filesystem::path& ffmpeg_path,
                                        const std::filesystem::path& source_path,
                                        const KeyframeEmbeddingItem& item) {
  // The checks decode_frames_at_timestamps_streaming makes before it decodes.
  if (!ffmpeg_executable_available(ffmpeg_path) || item.width <= 0 || item.height <= 0) {
    return {};
  }
  std::string decode_error;
  std::vector<Srgb8Pixel> pixels = decode_rgb_frame_at(ffmpeg_path, source_path, item.pts_us,
                                                       item.width, item.height, decode_error);
  if (pixels.empty()) {
    return {};
  }
  const ColorRasterFrame frame{"", item.pts_us, item.width, item.height, true,
                               std::move(pixels)};
  cv::Mat resized;
  cv::resize(srgb8_frame_to_cv_mat(frame), resized,
             cv::Size(kVisionEmbeddingInputSide, kVisionEmbeddingInputSide));
  return frame_to_clip_normalized_chw(resized);
}

std::optional<std::vector<float>> embed_keyframe_input(const svp::models::OnnxSession& session,
                                                       const std::vector<float>& input,
                                                       std::uint32_t embedding_dim) {
  if (input.empty()) {
    return std::nullopt;
  }
  try {
    std::vector<float> vector = session.run_visual_embedding(
        input.data(), input.size(), kVisionEmbeddingInputSide, kVisionEmbeddingInputSide);
    if (vector.size() != embedding_dim) {
      return std::nullopt;
    }
    l2_normalize(vector);
    return vector;
  } catch (...) {
    return std::nullopt;
  }
}

KeyframeEmbeddingOutcome embed_keyframe(const svp::models::OnnxSession& session,
                                        const std::filesystem::path& ffmpeg_path,
                                        const std::filesystem::path& source_path,
                                        const KeyframeEmbeddingItem& item,
                                        std::uint32_t embedding_dim) {
  KeyframeEmbeddingOutcome outcome;
  std::vector<float> input;
  try {
    input = keyframe_model_input(ffmpeg_path, source_path, item);
  } catch (...) {
    input.clear();
  }
  if (std::optional<std::vector<float>> vector =
          embed_keyframe_input(session, input, embedding_dim)) {
    outcome.embedded = true;
    outcome.vector = std::move(*vector);
  }
  return outcome;
}

}  // namespace svp::vision
