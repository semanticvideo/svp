#pragma once

// TaskArtifactAccess for whole-stage tasks run in this process. Stage tasks
// take no artifact inputs (they read the shared staging directory and their
// dependencies' committed states), and hand their encoded outputs here
// before returning; the attempt runner reads them back once, right after the
// task returns, to commit them.

#include "svp/exec/task_artifact_access.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace svp::builder::engine {

class StageOutputAccess final : public svp::exec::TaskArtifactAccess {
 public:
  // Holds `payloads` for the task's next read_outputs (replacing any left by
  // an abandoned attempt).
  void stage(const std::string& task_id, std::vector<svp::exec::FramePayload> payloads);

  // Throws ExecError(unresolved_input) for a spec with inputs.
  [[nodiscard]] svp::exec::ResolvedInputs resolve_inputs(
      const svp::exec::TaskSpec& spec) override;
  // Throws std::runtime_error when nothing is staged for the task or the
  // count differs from the result's outputs.
  [[nodiscard]] std::vector<svp::exec::FramePayload> read_outputs(
      const svp::exec::TaskResult& result) override;

 private:
  std::mutex mutex_;
  std::map<std::string, std::vector<svp::exec::FramePayload>> pending_;
};

}  // namespace svp::builder::engine
