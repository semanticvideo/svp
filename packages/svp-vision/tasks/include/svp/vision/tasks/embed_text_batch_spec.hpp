#pragma once

#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/tasks/embed_text_batch_parameters.hpp"
#include "svp/vision/tasks/item_batch_policy.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Coordinator side of embed.text_batch.

inline constexpr std::string_view kEmbedTextBatchLane = "embed.text_batch";

// Peak RSS of one task (admission, plan §3.5): a worker process holding
// nomic-embed-text v1.5 on ONNX Runtime CPU peaked at 712 MiB embedding OCR
// lines (svp-vision-dispatched-work-tests, SVP_DISPATCH_TEST_PEAK_RSS); the
// model's activations grow with sequence length up to
// kTextEmbeddingMaxTokens, so the estimate leaves about 40% above that.
inline constexpr std::uint64_t kEmbedTextBatchEstimatedPeakRssMb = 1024;

struct EmbedTextBatchTaskInputs {
  std::string build_session_id;
  std::vector<std::string> depends_on;
  // The text model's bundle (cached_model_ref).
  svp::exec::TaskModelRef model_ref;
  OnnxModelParameters model;
  std::uint32_t embedding_dim = 0;
  ItemBatchPolicy batch_policy;
};

// The validated TaskSpec for items [batch.first, batch.first + batch.count)
// of `items` (ordinals are positions in `items`). Throws
// std::invalid_argument for a batch outside `items`.
[[nodiscard]] svp::exec::TaskSpec make_embed_text_batch_task_spec(
    const EmbedTextBatchTaskInputs& inputs, const std::vector<TextEmbeddingItem>& items,
    const ItemBatch& batch);

[[nodiscard]] svp::exec::TaskOrderKey embed_text_batch_order_key(const ItemBatch& batch);

// Decodes a committed embed.text_batch result: one outcome per spec item, in
// spec order. Throws std::invalid_argument for outputs that do not match the
// spec.
[[nodiscard]] std::vector<TextEmbeddingOutcome> read_embed_text_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads);

}  // namespace svp::vision::tasks
