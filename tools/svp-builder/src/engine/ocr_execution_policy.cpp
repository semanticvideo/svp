#include "engine/ocr_execution_policy.hpp"

namespace svp::builder::engine {

svp::exec::TaskTypePolicy ocr_frame_batch_task_policy() {
  return svp::exec::TaskTypePolicy{.lease = svp::exec::LeasePolicy{},
                                    .max_attempts = svp::exec::kDefaultMaxAttempts};
}

}  // namespace svp::builder::engine
