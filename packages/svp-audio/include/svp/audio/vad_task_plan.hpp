#pragma once

#include "svp/media/media_probe.hpp"

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

struct VadTaskBoundary {
  std::string task_id;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::vector<std::string> depends_on;
  std::vector<std::string> input_refs;
  std::vector<std::string> output_refs;
  std::vector<std::string> model_refs;
  std::string processor_id;
};

struct VadTaskPlan {
  std::string processor_id = "proc_silero_vad_0001";
  std::string model_id = "model_silero_vad";
  std::string runtime = "onnxruntime";
  std::string execution_provider = "unresolved";
  std::int64_t chunk_duration_us = 30000000;
  std::int64_t chunk_overlap_us = 0;
  std::vector<VadTaskBoundary> tasks;
  std::vector<std::string> blockers;
};

[[nodiscard]] VadTaskPlan build_vad_task_plan(
    const svp::media::MediaProbe& probe,
    std::optional<std::int64_t> duration_us,
    bool model_runtime_available);

[[nodiscard]] nlohmann::json vad_task_plan_to_json(const VadTaskPlan& plan);

}  // namespace svp::audio
