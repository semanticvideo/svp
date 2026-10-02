#pragma once

#include "svp/vision/tasks/onnx_model_parameters.hpp"
#include "svp/vision/text_embedding_work.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Task type identity (plan §4.2): a batch of text observations to embed
// (svp/vision/text_embedding_work.hpp).
inline constexpr std::string_view kEmbedTextBatchTaskType = "embed.text_batch";
inline constexpr std::uint64_t kEmbedTextBatchTaskTypeVersion = 1;
// Outputs: one record per item (JSONL), and the vectors back to back as
// little-endian float32.
inline constexpr std::string_view kEmbedTextRecordsRole = "embed_text_records";
inline constexpr std::string_view kEmbedTextVectorsRole = "embed_text_vectors";

struct OrderedTextEmbeddingItem {
  // Position of the observation in the stage's list.
  std::uint64_t ordinal = 0;
  TextEmbeddingItem item;

  bool operator==(const OrderedTextEmbeddingItem&) const = default;
};

struct EmbedTextBatchParameters {
  std::vector<OrderedTextEmbeddingItem> items;
  OnnxModelParameters model;
  std::uint32_t embedding_dim = 0;
};

// Canonical parameters object:
//   {"embedding_dim","execution_provider",
//    "items":[{"id","ordinal","text"}, ...],"model_id",
//    "threads":{"inter_op","intra_op"}}
// Throws std::invalid_argument when the values break the schema below.
[[nodiscard]] nlohmann::json embed_text_batch_parameters_to_json(
    const EmbedTextBatchParameters& parameters);
[[nodiscard]] EmbedTextBatchParameters embed_text_batch_parameters_from_json(
    const nlohmann::json& value);

// nullopt when valid: known fields only, items non-empty with strictly
// ascending ordinals and string ids and texts, embedding_dim >= 1, a
// canonical model id, a known execution provider, explicit thread counts.
[[nodiscard]] std::optional<std::string> validate_embed_text_batch_parameters(
    const nlohmann::json& value);

// The parameters bytes one item adds to a spec, for batch sizing.
[[nodiscard]] std::uint64_t embed_text_item_parameter_bytes(const OrderedTextEmbeddingItem& item);

}  // namespace svp::vision::tasks
