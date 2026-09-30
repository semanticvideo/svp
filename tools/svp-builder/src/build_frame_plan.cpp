#include "build_frame_plan.hpp"

#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/visual_tracking_quality.hpp"

#include <utility>

namespace svp::builder {

svp::vision::FramePlanInputs build_frame_plan_inputs(
    const BuildStageExecutionPlan& stage_plan,
    const svp::media::MediaIngestPlan& media_plan,
    std::string_view visual_tracking_quality,
    bool model_runtime_available,
    std::optional<std::vector<std::int64_t>> ocr_diagnostic_override) {
  svp::vision::FramePlanInputs inputs =
      svp::vision::frame_plan_inputs_for_media(media_plan);

  inputs.color = stage_plan.run_foundation_color;
  inputs.canonical =
      stage_plan.run_package_skeleton || stage_plan.run_foundation_ocr;
  inputs.ocr = inputs.canonical;
  // The OCR stages run with default OCR generation options.
  inputs.ocr_sampling = svp::vision::OcrGenerationOptions{}.sampling_config;
  inputs.ocr_diagnostic_override = std::move(ocr_diagnostic_override);

  if (stage_plan.run_package_skeleton && model_runtime_available) {
    // An unparseable quality makes the package stage throw before decoding;
    // plan no tracking frames for it.
    inputs.visual_tracking =
        svp::vision::parse_visual_tracking_quality(visual_tracking_quality)
            .value_or(svp::vision::VisualTrackingQuality::off);
  }
  return inputs;
}

void plan_build_frames(svp::vision::FrameCatalog& catalog,
                       const BuildStageExecutionPlan& stage_plan,
                       const svp::media::MediaIngestPlan& media_plan,
                       std::string_view visual_tracking_quality,
                       bool model_runtime_available) {
  const svp::vision::FramePlanInputs inputs = build_frame_plan_inputs(
      stage_plan, media_plan, visual_tracking_quality,
      model_runtime_available,
      svp::vision::ocr_diagnostic_timestamp_override());
  svp::vision::load_frame_plan(
      catalog, svp::vision::plan_frame_registrations(inputs));
}

}  // namespace svp::builder
