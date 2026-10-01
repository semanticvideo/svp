#pragma once

#include "svp/exec/task_artifact_access.hpp"
#include "svp/exec/task_registry.hpp"

namespace svp::builder::workers {

// The task types this svp-builder runs for a coordinator (plan §3.1 rule 1:
// each task type is a C++ function in svp-builder, and local and remote
// execution call the same function from the same runtime). The pipeline's
// task types (OCR frame batches, crops, tracking windows, ASR chunks) are
// registered here as their stages move onto the task graph; until then a
// worker session accepts the protocol and answers any ASSIGN with the
// registry's unknown_task_type failure, never with arbitrary code.
void register_builder_worker_task_types(svp::exec::TaskTypeRegistry& registry,
                                        svp::exec::TaskArtifactAccess& artifacts);

}  // namespace svp::builder::workers
