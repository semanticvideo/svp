#pragma once

#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/ocr_temporal_sampling.hpp"
#include "svp/vision/visual_tracking_quality.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace svp::media { struct MediaIngestPlan; }

namespace svp::vision {

// One frame registration a stage will make, in stage execution order.
struct PlannedFrameRegistration {
  std::int64_t timestamp_us = 0;
  std::string purpose;
  bool keyframe = false;
};

// Plan-time facts that decide which frames the frame-decoding stages request.
// Every field is known before any stage runs; nothing here depends on decoded
// content or on stage results.
struct FramePlanInputs {
  std::int64_t duration_us = 0;
  int canonical_width = 0;
  int canonical_height = 0;
  int ocr_frame_width = 0;
  int ocr_frame_height = 0;

  // Stages that will run in this build.
  bool color = false;
  bool canonical = false;
  bool ocr = false;
  VisualTrackingQuality visual_tracking = VisualTrackingQuality::off;

  OcrSamplingConfig ocr_sampling;
  std::optional<std::vector<std::int64_t>> ocr_diagnostic_override;
};

// Fills the media-derived fields (duration, canonical raster, OCR decode
// dimensions). Stage selection is left to the caller.
[[nodiscard]] FramePlanInputs frame_plan_inputs_for_media(
    const media::MediaIngestPlan& media_plan);

// The registrations the stages make, in today's execution order: color
// schedule, canonical schedule, OCR samples, then visual tracking windows in
// window order. Each stage's first frame carries keyframe=true, as the stage
// decoders flag it. Duplicate timestamps across stages are kept; FrameCatalog
// folds them onto the first registration exactly as stage registration does.
//
// The plan assumes every requested frame decodes. A frame that fails to
// decode keeps its planned ID and is omitted from FrameCatalog::entries().
[[nodiscard]] std::vector<PlannedFrameRegistration> plan_frame_registrations(
    const FramePlanInputs& inputs);

// Registers the plan into an empty catalog and locks it, so stage
// registrations become lookups. Throws std::logic_error if the catalog
// already holds frames.
void load_frame_plan(FrameCatalog& catalog,
                     const std::vector<PlannedFrameRegistration>& plan);

}  // namespace svp::vision
