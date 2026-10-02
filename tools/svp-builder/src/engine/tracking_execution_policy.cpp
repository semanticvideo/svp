#include "engine/tracking_execution_policy.hpp"

namespace svp::builder::engine {

svp::exec::TaskTypePolicy track_window_task_policy(std::size_t worker_executors) {
  return svp::exec::TaskTypePolicy{
      .lease = svp::exec::LeasePolicy{},
      .max_attempts = svp::exec::kDefaultMaxAttempts + worker_executors};
}

}  // namespace svp::builder::engine
