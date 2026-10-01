#pragma once

// Turns the vision lane's (stage name, current, total) callbacks into build
// progress events: a stage starts on its first callback and completes when
// it reports current >= total. finish() completes stages that started but
// never reported their total.

#include "build_pipeline_internal.hpp"
#include "svp/package/spatial_embedding_placeholders.hpp"

#include <mutex>
#include <string>
#include <unordered_set>

namespace svp::builder {

class VisionStageProgress {
 public:
  explicit VisionStageProgress(BuildPipelineContext& context);
  VisionStageProgress(const VisionStageProgress&) = delete;
  VisionStageProgress& operator=(const VisionStageProgress&) = delete;

  // Valid while this object lives. Thread-safe.
  [[nodiscard]] svp::package::SpatialProgressCallback callback();
  void finish();

 private:
  void report(const char* stage, std::size_t current, std::size_t total,
              const char* message);

  BuildPipelineContext& context_;
  std::mutex mutex_;
  std::unordered_set<std::string> started_;
  std::unordered_set<std::string> completed_;
};

}  // namespace svp::builder
