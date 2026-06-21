#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::vision {

// A single evidence crop linked to a reconciled text region.
// Stores the source pixels behind an OCR observation so that
// uncertain OCR text can be inspected visually without re-decoding
// the full source media.
struct EvidenceCropRecord {
  std::string crop_id;
  std::string text_region_id;
  std::string text_observation_id;

  // Source frame provenance
  std::string source_frame_id;
  std::int64_t source_timestamp_us = 0;

  // Original bbox in OCR-frame pixel coordinates
  int original_bbox_left = 0;
  int original_bbox_top = 0;
  int original_bbox_right = 0;
  int original_bbox_bottom = 0;

  // Crop bbox in OCR-frame space (after margin expansion, before scaling)
  int crop_bbox_ocr_left = 0;
  int crop_bbox_ocr_top = 0;
  int crop_bbox_ocr_right = 0;
  int crop_bbox_ocr_bottom = 0;

  // Crop bbox in source-frame space (after coordinate transform)
  int crop_bbox_left = 0;
  int crop_bbox_top = 0;
  int crop_bbox_right = 0;
  int crop_bbox_bottom = 0;

  // OCR frame dimensions (resolution at which OCR was run)
  int ocr_frame_width = 0;
  int ocr_frame_height = 0;

  // Source frame dimensions (original video resolution)
  int source_frame_width = 0;
  int source_frame_height = 0;

  // Canonical raster dimensions (normalized bbox_px coordinate space)
  int canonical_raster_width = 0;
  int canonical_raster_height = 0;

  // Coordinate space of original_bbox: "ocr_frame"
  std::string bbox_coordinate_space;

  // Scale factor applied to transform from OCR-frame to source-frame
  double transform_scale_x = 1.0;
  double transform_scale_y = 1.0;

  // Crop extraction method (e.g. "ffmpeg_crop_scaled_to_source")
  std::string crop_extraction_method;

  // Evidence quality: "strong", "weak", "unsupported", "not_checked"
  std::string evidence_quality;
  std::string evidence_quality_reason;

  // ROI OCR re-read result on the saved crop
  std::string roi_ocr_text;
  double roi_ocr_confidence = 0.0;
  int roi_ocr_word_count = 0;

  // Legacy alias for OCR frame dimensions (backward compat)
  int frame_width = 0;
  int frame_height = 0;

  // Transform / preprocessing applied to the crop
  std::string crop_transform;

  // Image format: "jpeg" or "png"
  std::string image_format;

  // Relative path within the package (e.g. "text/evidence_crops/crop_000001.jpg")
  std::string crop_file_path;

  std::int64_t crop_size_bytes = 0;
  std::string blake3_hash;

  // Why this crop was selected
  // "representative", "best_confidence", "first_detection"
  std::string selection_reason;
};

// Result of running ROI OCR on a crop image.
// Currently not used (PP-OCR only runs on full frames), but kept
// for future per-crop re-read capabilities.
struct RoiOcrResult {
  std::string raw_text;
  double confidence = 0.0;
  std::string preprocessing_variant;
  int psm_used = 0;
  bool succeeded = false;
  int word_count = 0;
};

// Options controlling evidence crop generation.
struct EvidenceCropOptions {
  std::filesystem::path ffmpeg_path;
  std::filesystem::path source_media_path;

  // OCR frame resolution (the resolution at which OCR was run)
  int ocr_frame_width = 0;
  int ocr_frame_height = 0;

  // Source video resolution (for coordinate transform)
  int source_frame_width = 0;
  int source_frame_height = 0;

  // Canonical raster dimensions (for metadata auditing)
  int canonical_raster_width = 0;
  int canonical_raster_height = 0;

  // Maximum total number of crops across all regions
  std::size_t max_total_crops = 50;

  // Maximum total bytes for all crop images
  std::int64_t max_total_crop_bytes = 2 * 1024 * 1024;

  // Image format for crops: "jpeg" or "png"
  std::string crop_image_format = "jpeg";

  // JPEG quality (1-100, only used when format is jpeg)
  int jpeg_quality = 85;
};

// Aggregate result of evidence crop generation.
struct EvidenceCropResult {
  std::vector<EvidenceCropRecord> crops;
  std::int64_t total_crop_bytes = 0;
  bool crops_written = false;
  std::int64_t crop_count = 0;
  std::int64_t crops_skipped_count = 0;
  std::string crops_skipped_reason;

  // ROI OCR results, one per reconciled observation (parallel index).
  // If ROI OCR improved the result, the caller should use this text.
  std::vector<RoiOcrResult> roi_ocr_results;
};

// Input for crop generation, one per reconciled text observation.
struct CropGenerationInput {
  std::string text_region_id;
  std::string text_observation_id;
  std::string source_frame_id;
  std::int64_t source_timestamp_us = 0;
  int bbox_left = 0, bbox_top = 0, bbox_right = 0, bbox_bottom = 0;
  int frame_width = 0, frame_height = 0;
  double confidence = 0.0;
  int detection_count = 1;
  // The observation's raw text, used for evidence quality self-check
  std::string observation_raw_text;
};

[[nodiscard]] nlohmann::json evidence_crop_to_json(const EvidenceCropRecord& record);

[[nodiscard]] nlohmann::json evidence_crop_result_to_json(const EvidenceCropResult& result);

// Generate evidence crops for reconciled text observations.
// Extracts crop images from the source video, computes BLAKE3 hashes,
// and writes crop metadata + images.
[[nodiscard]] EvidenceCropResult generate_evidence_crops_internal(
    const EvidenceCropOptions& options,
    const std::vector<CropGenerationInput>& inputs,
    const std::filesystem::path& staging_dir);

}  // namespace svp::vision
