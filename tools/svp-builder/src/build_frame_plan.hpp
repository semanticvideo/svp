#pragma once

#include "svp/builder/build_pipeline.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/frame_plan.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace svp::builder {

// Maps the build's stage selection onto the frame plan inputs:
//   - color runs for --stop-after foundation-color and for full packages;
//   - canonical frames and OCR samples run for foundation-ocr and packages;
//   - visual tracking runs only inside the package stage, and only when the
//     ONNX model runtime is available (the package vision stage skips it
//     otherwise).
[[nodiscard]] svp::vision::FramePlanInputs build_frame_plan_inputs(
    const BuildStageExecutionPlan& stage_plan,
    const svp::media::MediaIngestPlan& media_plan,
    std::string_view visual_tracking_quality,
    bool model_runtime_available,
    std::optional<std::vector<std::int64_t>> ocr_diagnostic_override);

// Registers every frame the build's stages will decode into the catalog, in
// stage order, and locks it before any stage runs.
void plan_build_frames(svp::vision::FrameCatalog& catalog,
                       const BuildStageExecutionPlan& stage_plan,
                       const svp::media::MediaIngestPlan& media_plan,
                       std::string_view visual_tracking_quality,
                       bool model_runtime_available);

}  // namespace svp::builder
