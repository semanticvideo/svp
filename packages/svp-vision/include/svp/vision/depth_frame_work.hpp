#pragma once

// Per-frame work of the depth stage (dispatched_work.hpp): normalize one
// canonical frame, run the depth model, resize its output back to the frame,
// and convert it to the uint16 relative inverse depth payload.
// generate_depth_blocks runs exactly this function for each frame, so a
// payload computed by a depth.frame_batch task is the payload the stage
// itself would have written.

#include "svp/models/runtime.hpp"
#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/color_frame_sampling.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {

enum class DepthFrameStatus {
  ok,
  // The model run threw; `error` holds its message.
  inference_failed,
  // The model output did not give a usable width x height field (wrong
  // size, non-finite values, or constant depth).
  unusable_output,
};

struct DepthFrameOutcome {
  DepthFrameStatus status = DepthFrameStatus::ok;
  // width * height values when ok.
  std::vector<std::uint16_t> depth;
  std::string error;
  // For unusable_output: the model's element count, and the count after
  // resizing it to the frame.
  std::uint64_t output_elements = 0;
  std::uint64_t resized_elements = 0;

  bool operator==(const DepthFrameOutcome&) const = default;
};

// One frame, as the depth stage processes it.
[[nodiscard]] DepthFrameOutcome infer_depth_frame_outcome(const svp::models::OnnxSession& session,
                                                          const ColorRasterFrame& frame);

// Runs every frame with `model` and returns one outcome per frame, in frame
// order; nullopt when the frames must run in the stage itself.
// `on_progress(done, total)` is called as outcomes arrive. Throws
// DispatchedWorkError when it cannot deliver.
using DepthFrameDispatcher = std::function<std::optional<std::vector<DepthFrameOutcome>>(
    const std::vector<ColorRasterFrame>& frames, const DispatchedModel& model,
    const std::function<void(std::size_t done, std::size_t total)>& on_progress)>;

}  // namespace svp::vision
