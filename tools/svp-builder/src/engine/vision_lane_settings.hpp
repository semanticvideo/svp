#pragma once

// The vision lane settings a build's vision tasks run with, from the build's
// plan-time inputs. One definition, so the OCR frame-batch plan and the vision
// stage tasks can never disagree about OCR options.

#include "svp/builder/build_pipeline.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/package/vision_lane_stages.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace svp::builder::engine {

[[nodiscard]] svp::package::VisionLaneSettings make_vision_lane_settings(
    const BuildPipelineOptions& options, const svp::media::MediaIngestPlan& media_plan,
    const nlohmann::json& media_plan_json, const std::filesystem::path& staging_dir,
    const svp::models::ThreadPlan& thread_plan, bool model_runtime_available);

}  // namespace svp::builder::engine
