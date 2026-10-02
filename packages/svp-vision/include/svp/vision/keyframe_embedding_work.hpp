#pragma once

// Per-shot work of the keyframe-embedding part of the embedding stage
// (dispatched_work.hpp): decode one shot's first frame at its analysis
// raster, resize and CLIP-normalize it, run the vision model, and
// L2-normalize the vector. The stage runs exactly these functions for each
// keyframe, so a vector computed by an embed.keyframe_batch task is the
// vector the stage itself would have written.

#include "svp/models/runtime.hpp"
#include "svp/vision/dispatched_work.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {

struct KeyframeEmbeddingItem {
  std::string shot_id;
  std::int64_t pts_us = 0;
  int width = 0;
  int height = 0;

  bool operator==(const KeyframeEmbeddingItem&) const = default;
};

struct KeyframeEmbeddingOutcome {
  // False when the keyframe could not be decoded or embedded (the stage then
  // leaves the shot out).
  bool embedded = false;
  std::vector<float> vector;

  bool operator==(const KeyframeEmbeddingOutcome&) const = default;
};

// The model input for one keyframe: the frame at pts_us decoded at width x
// height, resized to the model's square input, CLIP-normalized CHW. Empty
// when ffmpeg is unusable or yields no frame (a decode miss).
[[nodiscard]] std::vector<float> keyframe_model_input(const std::filesystem::path& ffmpeg_path,
                                                      const std::filesystem::path& source_path,
                                                      const KeyframeEmbeddingItem& item);

// The vision model's L2-normalized vector for a prepared input; nullopt when
// inference failed or returned another dimension.
[[nodiscard]] std::optional<std::vector<float>> embed_keyframe_input(
    const svp::models::OnnxSession& session,
    const std::vector<float>& input,
    std::uint32_t embedding_dim);

// keyframe_model_input, then embed_keyframe_input.
[[nodiscard]] KeyframeEmbeddingOutcome embed_keyframe(const svp::models::OnnxSession& session,
                                                      const std::filesystem::path& ffmpeg_path,
                                                      const std::filesystem::path& source_path,
                                                      const KeyframeEmbeddingItem& item,
                                                      std::uint32_t embedding_dim);

// Embeds every keyframe with `model` into `embedding_dim` values and returns
// one outcome per keyframe, in keyframe order; nullopt when the keyframes
// must be embedded in the stage itself. `on_progress(done, total)` is called
// as outcomes arrive. Throws DispatchedWorkError when it cannot deliver.
using KeyframeEmbeddingDispatcher =
    std::function<std::optional<std::vector<KeyframeEmbeddingOutcome>>(
        const std::vector<KeyframeEmbeddingItem>& items, const DispatchedModel& model,
        std::uint32_t embedding_dim,
        const std::function<void(std::size_t done, std::size_t total)>& on_progress)>;

}  // namespace svp::vision
