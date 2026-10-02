#pragma once

// Vision stage work that a build may run as tasks (plan §4.2, M4): the
// per-item work of evidence crops, text and keyframe embeddings, and depth.
//
// Each of those stages keeps its own code for everything around the per-item
// work (model checks, blockers, block writing, IDs, staged files), and runs
// the per-item work through one function per item (evidence_crop_work.hpp,
// text_embedding_work.hpp, keyframe_embedding_work.hpp, depth_frame_work.hpp).
// A stage given a dispatcher hands that per-item work to it instead, in item
// order, and then continues exactly as it would have:
//
//   * only successful item outcomes are taken from a dispatcher; an item the
//     dispatcher reports as failed is computed again here, by the stage's own
//     code, so every failure a package records is one this Mac produced (the
//     same failure a build without a dispatcher records);
//   * a dispatcher that returns nullopt asks the stage to do the per-item work
//     itself, as a build without a dispatcher does (the coordinator could not
//     start the work);
//   * a dispatcher that cannot deliver outcomes throws DispatchedWorkError,
//     which the stages never turn into a blocker: the build fails instead of
//     writing a package that differs from a local build's.
//
// A build without dispatchers (every local build) never reaches any of this.

#include "svp/models/thread_plan.hpp"

#include <stdexcept>
#include <string>

namespace svp::vision {

class DispatchedWorkError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The model a stage runs its per-item work with, exactly as the stage loads
// it, so dispatched work runs the same model with the same settings.
struct DispatchedModel {
  std::string model_id;
  std::string execution_provider;
  svp::models::OrtThreadCounts threads;

  bool operator==(const DispatchedModel&) const = default;
};

}  // namespace svp::vision
