#pragma once

#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/ocr_sample_plan.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace svp::vision {

// The work of one ocr.frame_batch task (plan §4.2): decode a slice of the OCR
// sample plan and run PP-OCR on each frame. Nothing here knows about tasks,
// workers, or frame IDs; the local OCR stage calls it in-process and the task
// function calls it on a worker, so both produce the same records.
struct OcrFrameBatchRequest {
  std::filesystem::path source_path;
  std::filesystem::path ffmpeg_path;
  int frame_width = 0;
  int frame_height = 0;
  // Plan slice, in ordinal order.
  std::vector<OcrSample> samples;
};

struct OcrFrameBatchHooks {
  // Runs before each sample is decoded. Throwing stops the batch: the task
  // function uses it for cooperative cancellation between frames.
  std::function<void()> before_sample;
  // Runs once a sample has decoded, before PP-OCR (progress reporting).
  std::function<void(const OcrSample&)> on_sample_decoded;
};

struct OcrFrameBatchOutcome {
  // False when no decode could be attempted at all (ffmpeg missing or a
  // non-positive frame size); `samples` is then empty and skipped_reason says
  // why, worded as the frame decoders word it.
  bool decoding_attempted = false;
  std::string skipped_reason;
  // One record per request sample, in request order.
  std::vector<OcrSampleDetections> samples;
};

// `session` must be available. PP-OCR exceptions are recorded per sample
// (ocr_failed), not thrown; exceptions from hooks propagate.
[[nodiscard]] OcrFrameBatchOutcome run_ocr_frame_batch(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const OcrFrameBatchRequest& request,
    const OcrFrameBatchHooks& hooks = {});

// PP-OCR on one already decoded frame, as a sample record (ok, frame_invalid,
// or ocr_failed). Shared by run_ocr_frame_batch and the canonical-frame
// fallback so both classify frames identically.
[[nodiscard]] OcrSampleDetections run_ocr_on_decoded_frame(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const ColorRasterFrame& frame,
    std::uint64_t sample_ordinal);

}  // namespace svp::vision
