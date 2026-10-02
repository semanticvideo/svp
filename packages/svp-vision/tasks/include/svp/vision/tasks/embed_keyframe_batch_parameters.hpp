#pragma once

#include "svp/vision/keyframe_embedding_work.hpp"
#include "svp/vision/tasks/onnx_model_parameters.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Task type identity (plan §4.2): a batch of shot keyframes to embed
// (svp/vision/keyframe_embedding_work.hpp).
inline constexpr std::string_view kEmbedKeyframeBatchTaskType = "embed.keyframe_batch";
inline constexpr std::uint64_t kEmbedKeyframeBatchTaskTypeVersion = 1;
// TaskSpec input holding the source media bytes (role
// kOcrFrameBatchSourceRole).
inline constexpr std::string_view kEmbedKeyframeBatchSourceInput = "source";
// Outputs: one record per keyframe (JSONL), and the vectors back to back as
// little-endian float32.
inline constexpr std::string_view kEmbedKeyframeRecordsRole = "embed_keyframe_records";
inline constexpr std::string_view kEmbedKeyframeVectorsRole = "embed_keyframe_vectors";

struct OrderedKeyframeItem {
  // Position of the keyframe in the stage's list.
  std::uint64_t ordinal = 0;
  KeyframeEmbeddingItem item;

  bool operator==(const OrderedKeyframeItem&) const = default;
};

struct EmbedKeyframeBatchParameters {
  std::vector<OrderedKeyframeItem> keyframes;
  OnnxModelParameters model;
  std::uint32_t embedding_dim = 0;
  // ffmpeg_build_identity() of the coordinator's ffmpeg ("b3:<hex>").
  std::string ffmpeg_build;
};

// Canonical parameters object:
//   {"decode":{"ffmpeg_build"},"embedding_dim","execution_provider",
//    "keyframes":[{"height","ordinal","pts_us","shot_id","width"}, ...],
//    "model_id","threads":{"inter_op","intra_op"}}
// Throws std::invalid_argument when the values break the schema below.
[[nodiscard]] nlohmann::json embed_keyframe_batch_parameters_to_json(
    const EmbedKeyframeBatchParameters& parameters);
[[nodiscard]] EmbedKeyframeBatchParameters embed_keyframe_batch_parameters_from_json(
    const nlohmann::json& value);

// nullopt when valid: known fields only, keyframes non-empty with strictly
// ascending ordinals, non-negative timestamps, a decode size of at least
// 1x1, embedding_dim >= 1, the model fields, and a decoder "b3:<64 hex>".
[[nodiscard]] std::optional<std::string> validate_embed_keyframe_batch_parameters(
    const nlohmann::json& value);

[[nodiscard]] std::uint64_t embed_keyframe_parameter_bytes(const OrderedKeyframeItem& item);

}  // namespace svp::vision::tasks
