#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/depth_frame_work.hpp"
#include "svp/vision/tasks/depth_frame_batch_parameters.hpp"
#include "svp/vision/tasks/item_batch_policy.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Coordinator side of depth.frame_batch.

inline constexpr std::string_view kDepthFrameBatchLane = "depth.frame_batch";

// Peak RSS of one task (admission, plan §3.5): a worker process holding
// Depth Anything V2 Small peaked at 379 MiB on 640x360 canonical frames
// (svp-vision-dispatched-work-tests, SVP_DISPATCH_TEST_PEAK_RSS); canonical
// frames never exceed kCanonicalLongestDisplayDimension on their long side,
// so the estimate leaves about 35% above the measurement.
inline constexpr std::uint64_t kDepthFrameBatchEstimatedPeakRssMb = 512;

struct DepthFrameBatchTaskInputs {
  std::string build_session_id;
  std::vector<std::string> depends_on;
  // The source media, role kOcrFrameBatchSourceRole.
  svp::exec::ArtifactRef source;
  // The depth model's bundle (cached_model_ref).
  svp::exec::TaskModelRef model_ref;
  OnnxModelParameters model;
  std::string ffmpeg_build;
  ItemBatchPolicy batch_policy;
};

// The validated TaskSpec for frames [batch.first, batch.first + batch.count)
// of `frames`. Throws std::invalid_argument for a batch outside `frames`.
[[nodiscard]] svp::exec::TaskSpec make_depth_frame_batch_task_spec(
    const DepthFrameBatchTaskInputs& inputs, const std::vector<DepthFrameItem>& frames,
    const ItemBatch& batch);

[[nodiscard]] svp::exec::TaskOrderKey depth_frame_batch_order_key(const ItemBatch& batch);

// Bytes of one frame's depth field in a result (uint16 per pixel).
[[nodiscard]] std::uint64_t depth_frame_field_bytes(const DepthFrameItem& frame);

// Decodes a committed depth.frame_batch result: one outcome per spec frame,
// in spec order. A frame the executor could not decode to the spec's pixels
// comes back as inference_failed with an empty error, which the stage runs
// itself like any failed frame. Throws std::invalid_argument for outputs
// that do not match the spec.
[[nodiscard]] std::vector<DepthFrameOutcome> read_depth_frame_batch_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads);

}  // namespace svp::vision::tasks
