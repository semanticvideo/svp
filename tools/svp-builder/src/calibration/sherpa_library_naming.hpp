#pragma once

// Naming the sherpa-onnx library a measurement of diarize.window is taken
// against (audio_capacity_workloads.hpp AudioCalibrationSetup), found exactly
// as a build of the runtime installed with `executable` finds it without
// --sherpa-lib: SHERPA_ONNX_LIB_PATH, then the runtime bundle's pinned
// library (offer_bundled_sherpa_library), then the unpinned locations. The
// library is named, never loaded: sherpa-onnx must not load before ONNX
// Runtime models (svp/audio/sherpa_diarization.hpp).

#include "calibration/audio_capacity_workloads.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace svp::builder::calibration {

// Sets `setup.sherpa_library` when `setup` dispatches diarize.window and a
// library is found. Otherwise (none found, or the bundle next to `executable`
// is broken or lists a library it lacks) clears its diarization model ref,
// so diarize.window is not measured, and returns why. Never throws for a
// library problem: diarize.window is one best-effort type among many.
[[nodiscard]] std::optional<std::string> name_diarization_library(
    AudioCalibrationSetup& setup, const std::filesystem::path& executable);

}  // namespace svp::builder::calibration
