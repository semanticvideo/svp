#pragma once

#include "svp/vision/ocr_sample_plan.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Task type identity (plan §4.2). The version changes whenever the parameter
// schema, the payload format, or the meaning of either changes.
inline constexpr std::string_view kOcrFrameBatchTaskType = "ocr.frame_batch";
inline constexpr std::uint64_t kOcrFrameBatchTaskTypeVersion = 1;
// TaskSpec input holding the source media bytes.
inline constexpr std::string_view kOcrFrameBatchSourceInput = "source";
inline constexpr std::string_view kOcrFrameBatchSourceRole = "source_media";
// The one output: OcrSampleDetections JSONL, one record per sample.
inline constexpr std::string_view kOcrFrameDetectionsRole = "ocr_frame_detections";
inline constexpr std::string_view kOcrFrameDetectionsMediaType = "application/x-ndjson";

// Everything an ocr.frame_batch task needs besides its source bytes and the
// worker's own model cache and ffmpeg: the plan slice, the decode size, and
// every PP-OCR value that can change output, thread counts included (plan
// §2.4 item 5). Workers never fill a value from their own host.
struct OcrFrameBatchParameters {
  std::vector<OcrSample> samples;
  int frame_width = 0;
  int frame_height = 0;
  // model_cache_root and manifest_filename are worker-local and not sent.
  PpOcrOptions pp_ocr;
};

// Canonical parameters object:
//   {"decode":{"frame_height","frame_width"},
//    "detector":{"box_thresh","execution_mode","graph_optimization_level",
//                "limit_side_len","model_id","threads":{"inter_op","intra_op"},
//                "thresh","unclip_ratio"},
//    "execution_provider",
//    "recognizer":{"execution_mode","graph_optimization_level","image_height",
//                  "max_width","min_text_score","model_id",
//                  "parallel_min_boxes","parallel_workers",
//                  "threads":{"inter_op","intra_op"}},
//    "samples":{"ordinals":[...],"timestamps_us":[...]}}
//
// Throws std::invalid_argument when the values break the schema checked by
// validate_ocr_frame_batch_parameters (for example a thread count left at
// kRuntimeChoosesThreadCount, which would let each worker pick its own).
// A graph optimization level ONNX Runtime does not name is sent as -1, the
// runtime default it already behaves as.
[[nodiscard]] nlohmann::json ocr_frame_batch_parameters_to_json(
    const OcrFrameBatchParameters& parameters);

// Strict inverse; throws std::invalid_argument with the validator's reason.
[[nodiscard]] OcrFrameBatchParameters ocr_frame_batch_parameters_from_json(
    const nlohmann::json& value);

// The registry's parameter validator (plan §4.3): nullopt when `value` is a
// valid parameters object, otherwise why not. Unknown, missing, or mistyped
// fields; samples that are empty, unequal in length, or not strictly
// ascending; a decode size outside 1..kOcrMaxFrameDimension; a thread or
// worker count below 1; and out-of-range thresholds are all rejected.
[[nodiscard]] std::optional<std::string> validate_ocr_frame_batch_parameters(
    const nlohmann::json& value);

}  // namespace svp::vision::tasks
