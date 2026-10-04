#pragma once

// What `workers pair`, `workers sync`, and `workers fleet pair` measure: the
// work a default --distributed build on this Mac dispatches, set up exactly
// as such a build sets it up (the same thread plan, model bundles, decoder,
// and tracking options), so the records they store are the ones the build
// looks for (calibration/calibration_plan.hpp).

#include "ocr_calibration_runs.hpp"
#include "track_window_calibration_runs.hpp"

#include "calibration/audio_capacity_workloads.hpp"
#include "calibration/calibration_plan.hpp"
#include "calibration/dispatched_capacity_workloads.hpp"

#include "svp/vision/visual_tracking_quality.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace svp::builder::workers {

struct DefaultBuildCalibration {
  OcrCalibrationSetup ocr;
  calibration::DispatchedCalibrationSetup dispatched;
  // sherpa_library names the library this Mac's builds load (not loaded
  // here); diarization_model_ref is unset when none can be named.
  calibration::AudioCalibrationSetup audio;
  // Per measured tracking quality whose bundles this Mac's cache holds.
  std::map<svp::vision::VisualTrackingQuality, TrackWindowCalibrationSetup> tracking;
  // Every model bundle those measurements load.
  std::vector<std::string> model_ids;
  // In measuring order.
  std::vector<calibration::CalibrationStep> steps;
  // Why a kind of work is not measured (bundles missing, ...), for the
  // command to report.
  std::vector<std::string> skipped;
};

// Throws std::runtime_error when OCR cannot be set up (ffmpeg or the PP-OCR
// bundles missing), as default_ocr_calibration_setup does.
[[nodiscard]] DefaultBuildCalibration default_build_calibration(
    const std::filesystem::path& model_cache);

}  // namespace svp::builder::workers
