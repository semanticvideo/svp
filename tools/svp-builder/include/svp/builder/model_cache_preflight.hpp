#pragma once

#include "svp/builder/build_pipeline.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace svp::builder {

class ModelCachePreflightError : public std::runtime_error {
 public:
  explicit ModelCachePreflightError(std::string message)
      : std::runtime_error(std::move(message)) {}
};

[[nodiscard]] bool schedules_model_backed_work(
    const BuildStageExecutionPlan& stage_plan) noexcept;

void verify_authoritative_model_cache(
    const std::filesystem::path& model_cache_dir);

}  // namespace svp::builder
