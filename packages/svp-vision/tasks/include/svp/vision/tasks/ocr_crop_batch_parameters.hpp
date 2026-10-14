#pragma once

#include "svp/vision/evidence_crop_work.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks {

// Task type identity (plan §4.2): a batch of evidence-crop jobs
// (svp/vision/evidence_crop_work.hpp). The version changes whenever the
// parameter schema, the payload format, or the meaning of either changes.
inline constexpr std::string_view kOcrCropBatchTaskType = "ocr.crop_batch";
inline constexpr std::uint64_t kOcrCropBatchTaskTypeVersion = 1;
// TaskSpec input holding the source media bytes (role
// kOcrFrameBatchSourceRole, the same source the OCR frame batches read).
inline constexpr std::string_view kOcrCropBatchSourceInput = "source";
// Outputs: one record per job (JSONL), and the crop images back to back.
inline constexpr std::string_view kOcrCropRecordsRole = "ocr_crop_records";
inline constexpr std::string_view kOcrCropImagesRole = "ocr_crop_images";

// Image formats extract_evidence_crop_image encodes.
inline constexpr std::string_view kEvidenceCropFormatJpeg = "jpeg";
inline constexpr std::string_view kEvidenceCropFormatPng = "png";
// JPEG quality is a percentage.
inline constexpr int kMaxJpegQuality = 100;

// Everything an ocr.crop_batch task needs besides its source bytes and the
// worker's own model cache and ffmpeg: the jobs, the decoder identity, and
// every PP-OCR value of the ROI re-read (thread counts explicit).
struct OcrCropBatchParameters {
  std::vector<EvidenceCropJob> jobs;
  // ffmpeg_build_identity() of the coordinator's ffmpeg ("b3:<hex>").
  std::string ffmpeg_build;
  // model_cache_root and manifest_filename are worker-local and not sent.
  PpOcrOptions pp_ocr;
};

// Canonical parameters object: the PP-OCR fields (pp_ocr_parameters.hpp) and
//   {"decode":{"ffmpeg_build"},
//    "jobs":[{"height","image_format","jpeg_quality","left","ordinal",
//             "seek_us","top","width"}, ...]}
// Throws std::invalid_argument when the values break the schema below.
[[nodiscard]] nlohmann::json ocr_crop_batch_parameters_to_json(
    const OcrCropBatchParameters& parameters);

// Strict inverse; throws std::invalid_argument with the validator's reason.
[[nodiscard]] OcrCropBatchParameters ocr_crop_batch_parameters_from_json(
    const nlohmann::json& value);

// nullopt when `value` is valid: known fields only, jobs non-empty with
// strictly ascending ordinals, non-negative seek and origin, a size of at
// least 1x1, a supported image format, a JPEG quality in 1..100, a decoder
// "b3:<64 hex>", and the PP-OCR rules of pp_ocr_parameters.hpp.
[[nodiscard]] std::optional<std::string> validate_ocr_crop_batch_parameters(
    const nlohmann::json& value);

}  // namespace svp::vision::tasks
