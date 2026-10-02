#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/tasks/embed_keyframe_batch_parameters.hpp"
#include "svp/vision/tasks/item_batch_policy.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Coordinator side of embed.keyframe_batch.

inline constexpr std::string_view kEmbedKeyframeBatchLane = "embed.keyframe_batch";

// Peak RSS of one task (admission, plan §3.5): a worker process holding
// nomic-embed-vision v1.5 peaked at 467 MiB embedding canonical-raster
// keyframes (svp-vision-dispatched-work-tests, SVP_DISPATCH_TEST_PEAK_RSS).
// The model input is a fixed 224x224, so only the decoded frame grows with
// the source's raster; the estimate leaves about 35% above the measurement.
inline constexpr std::uint64_t kEmbedKeyframeBatchEstimatedPeakRssMb = 640;

struct EmbedKeyframeBatchTaskInputs {
  std::string build_session_id;
  std::vector<std::string> depends_on;
  // The source media, role kOcrFrameBatchSourceRole.
  svp::exec::ArtifactRef source;
  // The vision model's bundle (cached_model_ref).
  svp::exec::TaskModelRef model_ref;
  OnnxModelParameters model;
  std::uint32_t embedding_dim = 0;
  std::string ffmpeg_build;
  ItemBatchPolicy batch_policy;
};

// The validated TaskSpec for keyframes [batch.first, batch.first +
// batch.count) of `keyframes`. Throws std::invalid_argument for a batch
// outside `keyframes`.
[[nodiscard]] svp::exec::TaskSpec make_embed_keyframe_batch_task_spec(
    const EmbedKeyframeBatchTaskInputs& inputs,
    const std::vector<KeyframeEmbeddingItem>& keyframes, const ItemBatch& batch);

[[nodiscard]] svp::exec::TaskOrderKey embed_keyframe_batch_order_key(const ItemBatch& batch);

// Decodes a committed embed.keyframe_batch result: one outcome per spec
// keyframe, in spec order. Throws std::invalid_argument for outputs that do
// not match the spec.
[[nodiscard]] std::vector<KeyframeEmbeddingOutcome> read_embed_keyframe_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads);

}  // namespace svp::vision::tasks
