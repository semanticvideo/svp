#pragma once

#include "svp/models/thread_plan.hpp"

#include <string>

namespace svp::vision::tasks {

// The model part of the parameters of a task type that runs one ONNX model
// (embed.text_batch, embed.keyframe_batch, depth.frame_batch): which model,
// on which execution provider, with which thread counts (explicit, plan
// §2.4 item 5). Canonical JSON fields:
//   "execution_provider", "model_id", "threads":{"inter_op","intra_op"}
struct OnnxModelParameters {
  std::string model_id;
  std::string execution_provider;
  svp::models::OrtThreadCounts threads;

  bool operator==(const OnnxModelParameters&) const = default;
};

}  // namespace svp::vision::tasks
