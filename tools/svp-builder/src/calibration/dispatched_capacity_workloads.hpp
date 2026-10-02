#pragma once

// The capacity calibration workloads of the dispatched vision task types
// (capacity_sweep.hpp; plan §3.5 "each worker measures throughput per task
// type"). Every one runs on the OCR calibration clip (ocr_calibration_clip
// .hpp), which every Mac decodes and reads the same way, so measurements of
// different Macs compare like for like. Each is sized so that one timed batch
// on one slot of an Apple Silicon Mac lasts a few seconds: long enough that
// scheduling noise of tens of milliseconds stays a few percent of a step,
// short enough that a whole sweep stays well under a minute per type:
//   * ocr.crop_batch: one crop per rendered text line of the clip's
//     kCropCalibrationFrame-th frame (48 lines), cut exactly as the OCR stage
//     cuts an observation's crop (evidence_crop_geometry) and encoded as the
//     stage encodes it (measured about 0.13 s per crop on an M4 mini);
//   * embed.text_batch: every rendered line's text, one item per line (180
//     items, about 15 ms each);
//   * embed.keyframe_batch: the clip's frames at the canonical analysis
//     raster of a source of the clip's size, each kKeyframeCalibrationRepeats
//     times (about 0.2 s each);
//   * depth.frame_batch: the same frames, each kDepthCalibrationRepeats times
//     (about 0.5 s each).

#include "calibration/capacity_sweep.hpp"

#include "svp/builder/distributed_execution.hpp"
#include "svp/exec/artifact_ref.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::calibration {

// Bumped whenever a workload below changes; capacity records name it so a
// changed workload invalidates old measurements.
inline constexpr std::uint32_t kDispatchedCalibrationRecipeVersion = 2;

// Workload sizes (see above): the clip frame whose lines are cropped, and
// how many times each of the clip's four frames appears in the keyframe and
// depth workloads.
inline constexpr std::size_t kCropCalibrationFrame = 2;
inline constexpr std::size_t kKeyframeCalibrationRepeats = 4;
inline constexpr std::size_t kDepthCalibrationRepeats = 2;

struct DispatchedCalibrationSetup {
  // The OCR work's PP-OCR, which the crop tasks' ROI re-read loads.
  svp::vision::PpOcrOptions pp_ocr;
  std::vector<svp::exec::TaskModelRef> pp_ocr_model_refs;
  DistributedVisionWork vision;
  std::string ffmpeg_build;
};

// The dispatched task types `vision` names, in a fixed order.
[[nodiscard]] std::vector<std::string> dispatched_task_types(const DistributedVisionWork& vision);

// Peak RSS of one task of `task_type` (its spec header's estimate), for the
// sweep's slot bound.
[[nodiscard]] std::uint64_t dispatched_task_peak_rss_mb(std::string_view task_type);

// The calibration workload of `task_type` on the clip `clip` (resolvable by
// the executor as role kOcrFrameBatchSourceRole). The depth workload decodes
// the clip's frames from `clip_file` with `ffmpeg` here, to name their pixels.
// Throws std::runtime_error when `setup` does not dispatch the type or the
// clip cannot be decoded.
[[nodiscard]] CapacityWorkload dispatched_capacity_workload(
    std::string_view task_type, const DispatchedCalibrationSetup& setup,
    const svp::exec::ArtifactRef& clip, const std::filesystem::path& clip_file,
    const std::filesystem::path& ffmpeg);

}  // namespace svp::builder::calibration
