#pragma once

// What this Mac brings to a worker session as coordinator: its runtime (the
// running svp-builder and, when installed, its bundle), the thread plan its
// tasks carry, and its model cache.

#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/runtime_source.hpp"
#include "svp/models/thread_plan.hpp"

#include <filesystem>
#include <optional>

namespace svp::builder::workers {

struct CoordinatorContext {
  svp::exec::worker::CoordinatorRuntime runtime;
  svp::models::ThreadPlan thread_plan;
  std::filesystem::path model_cache;
  std::optional<svp::exec::worker::ModelSetSummary> model_set;

  [[nodiscard]] svp::exec::worker::CoordinatorHello hello() const;
};

// The real path of the running executable.
[[nodiscard]] std::filesystem::path current_executable();

[[nodiscard]] CoordinatorContext load_coordinator_context();

}  // namespace svp::builder::workers
