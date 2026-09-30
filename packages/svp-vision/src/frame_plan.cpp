#include "svp/vision/frame_plan.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/real_frame_color_sampling.hpp"
#include "svp/vision/visual_entity_sampling.hpp"

#include <stdexcept>

namespace svp::vision {
namespace {

// Each stage decoder flags the first frame of its own request as a keyframe.
void append_stage(std::vector<PlannedFrameRegistration>& plan,
                  const std::vector<std::int64_t>& timestamps_us,
                  const char* purpose) {
  for (std::size_t i = 0; i < timestamps_us.size(); ++i) {
    plan.push_back({timestamps_us[i], purpose, i == 0});
  }
}

bool canonical_raster_positive(const FramePlanInputs& inputs) {
  return inputs.canonical_width > 0 && inputs.canonical_height > 0;
}

}  // namespace

FramePlanInputs frame_plan_inputs_for_media(
    const media::MediaIngestPlan& media_plan) {
  FramePlanInputs inputs;
  inputs.duration_us = compute_media_duration_us(media_plan);
  inputs.canonical_width = media_plan.canonical_raster.width;
  inputs.canonical_height = media_plan.canonical_raster.height;
  const OcrSourceFrameDimensions ocr_dims =
      ocr_decode_frame_dimensions(media_plan);
  inputs.ocr_frame_width = ocr_dims.width;
  inputs.ocr_frame_height = ocr_dims.height;
  return inputs;
}

std::vector<PlannedFrameRegistration> plan_frame_registrations(
    const FramePlanInputs& inputs) {
  std::vector<PlannedFrameRegistration> plan;

  // Color and canonical decodes skip entirely without a positive raster;
  // deterministic_seek_timestamps_us is empty for a non-positive duration.
  if (inputs.color && canonical_raster_positive(inputs)) {
    append_stage(plan,
                 deterministic_seek_timestamps_us(inputs.duration_us,
                                                  kColorDecodedFrameCount),
                 kColorFramePurpose);
  }
  if (inputs.canonical && canonical_raster_positive(inputs)) {
    append_stage(plan,
                 deterministic_seek_timestamps_us(inputs.duration_us,
                                                  kCanonicalDecodedFrameCount),
                 kCanonicalFramePurpose);
  }

  // OCR decodes its own samples only when its decode raster is positive;
  // otherwise it reuses the canonical frames and registers nothing new. The
  // diagnostic override applies even when the duration is unknown.
  if (inputs.ocr && inputs.ocr_frame_width > 0 &&
      inputs.ocr_frame_height > 0) {
    append_stage(plan,
                 plan_ocr_temporal_sampling(inputs.duration_us,
                                            inputs.ocr_sampling,
                                            inputs.ocr_diagnostic_override)
                     .timestamps_us,
                 kOcrFramePurpose);
  }

  // Visual tracking decodes each window at the canonical raster, in window
  // order, flagging each window's first frame.
  if (visual_tracking_enabled(inputs.visual_tracking) &&
      canonical_raster_positive(inputs)) {
    const auto windows = make_visual_entity_sampling_plan(
        inputs.duration_us,
        visual_entity_sampling_options(
            visual_tracking_quality_policy(inputs.visual_tracking)));
    for (const auto& window : windows) {
      append_stage(plan, window.timestamps_us,
                   kVisualEntityTrackingFramePurpose);
    }
  }

  return plan;
}

void load_frame_plan(FrameCatalog& catalog,
                     const std::vector<PlannedFrameRegistration>& plan) {
  if (!catalog.planned_entries().empty() || catalog.locked_to_plan()) {
    throw std::logic_error(
        "a frame plan can only be loaded into an empty, unlocked catalog");
  }
  for (const auto& frame : plan) {
    (void)catalog.register_frame(frame.timestamp_us, frame.purpose,
                                 frame.keyframe);
  }
  catalog.lock_to_plan();
}

}  // namespace svp::vision
