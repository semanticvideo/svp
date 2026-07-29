#pragma once

#include "svp/builder/build_pipeline.hpp"

#include <filesystem>

namespace svp::builder {

[[nodiscard]] bool schedules_model_backed_work(
    const BuildStageExecutionPlan& stage_plan) noexcept;

void verify_authoritative_model_cache(
    const std::filesystem::path& model_cache_dir);

}  // namespace svp::builder
