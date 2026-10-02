#pragma once

// The vision stage work a --distributed build may dispatch (M4,
// DistributedVisionWork): which models its embedding and depth stages load
// and with which settings, exactly as those stages load them
// (svp::package::run_vision_text_embedding_stage, run_vision_depth_stage:
// each model's default id and execution provider, the thread plan's role).
// A type is left out when its model bundle is not in this Mac's cache or its
// thread counts are not explicit (workers refuse thread counts left to each
// Mac, plan §2.4 item 5); its stage then does its work itself.

#include "svp/builder/distributed_execution.hpp"
#include "svp/models/thread_plan.hpp"

#include <filesystem>

namespace svp::builder::engine {

[[nodiscard]] DistributedVisionWork plan_distributed_vision_work(
    const std::filesystem::path& model_cache_root, const svp::models::ThreadPlan& thread_plan);

}  // namespace svp::builder::engine
