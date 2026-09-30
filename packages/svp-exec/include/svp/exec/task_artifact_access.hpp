#pragma once

#include "svp/exec/frame.hpp"
#include "svp/exec/resolved_inputs.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <vector>

namespace svp::exec {

// How a runtime moves bytes into and out of task functions. Task functions
// see inputs as verified local files (ResolvedInputs) and describe outputs by
// ArtifactRef only; the bytes behind those refs live wherever the runtime's
// store put them. The worker loop and the in-process executor use this seam
// to resolve a spec's inputs before running it and to read the output bytes
// that travel with the result. The content-addressed cache (plan §4.6) is the
// production implementation; tests use an in-memory store.
//
// Implementations must be safe to call from several task threads at once.
class TaskArtifactAccess {
 public:
  virtual ~TaskArtifactAccess() = default;

  // Throws ExecError(unresolved_input) when an input's verified bytes are not
  // available.
  [[nodiscard]] virtual ResolvedInputs resolve_inputs(const TaskSpec& spec) = 0;

  // Bytes of result.outputs[i] at index i. Throws when an output is missing.
  [[nodiscard]] virtual std::vector<FramePayload> read_outputs(
      const TaskResult& result) = 0;
};

}  // namespace svp::exec
