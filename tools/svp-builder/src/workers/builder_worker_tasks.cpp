#include "builder_worker_tasks.hpp"

namespace svp::builder::workers {

void register_builder_worker_task_types(svp::exec::TaskTypeRegistry& registry,
                                        svp::exec::TaskArtifactAccess& artifacts) {
  (void)registry;
  (void)artifacts;
}

}  // namespace svp::builder::workers
