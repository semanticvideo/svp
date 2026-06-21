#pragma once

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::media { struct MediaIngestPlan; }

namespace svp::vision {

struct OcrGenerationOptions {
  std::filesystem::path tesseract_path = "tesseract";
  std::filesystem::path ffmpeg_path = "ffmpeg";
  std::string language = "eng";
  // Minimum confidence threshold (0-100) for accepting a word from tesseract.
  int min_word_confidence = 30;
  // Maximum number of OCR text observations to produce per build run.
  std::size_t max_observations = 100;
  // When provided, OCR decodes its own frames at ocr_frame_width x ocr_frame_height
  // from the source media (higher resolution than canonical raster for text detection).
  // Bounding boxes are normalized to canonical_raster_width x canonical_raster_height.
  const svp::media::MediaIngestPlan* media_plan = nullptr;
  int ocr_frame_width = 0;
  int ocr_frame_height = 0;
  int canonical_raster_width = 0;
  int canonical_raster_height = 0;
  // When true, generate evidence crops for reconciled text regions.
  bool generate_evidence_crops = false;
  // Maximum crops per reconciled text region.
  int max_crops_per_region = 2;
  // Maximum total crops across all regions.
  std::size_t max_total_crops = 50;
  // Maximum total bytes for all crop images.
  std::int64_t max_total_crop_bytes = 2 * 1024 * 1024;
};

struct OcrGenerationResult {
  bool ocr_available = false;
  bool ocr_frame_input_available = false;
  bool ocr_detection_run = false;
  bool ocr_recognition_run = false;
  bool text_regions_written = false;
  bool text_observations_written = false;
  bool numeric_values_written = false;
  bool text_absence_written = false;
  std::int64_t text_region_count = 0;
  std::int64_t text_observation_count = 0;
  std::int64_t numeric_value_count = 0;
  std::string blocker;
  std::vector<TextRegionRecord> text_regions;
  std::vector<TextObservationRecord> text_observations;
  std::vector<NumericValueRecord> numeric_values;
  TextAbsenceRecord text_absence;
  std::vector<nlohmann::json> processors;
  // Evidence crops
  std::vector<EvidenceCropRecord> evidence_crops;
  std::int64_t evidence_crop_count = 0;
  std::int64_t evidence_crop_total_bytes = 0;
  bool evidence_crops_written = false;
  std::int64_t evidence_crops_skipped = 0;
  std::string evidence_crops_skipped_reason;
  // ROI hardening results, parallel to text_observations.
  // When ROI OCR produced better text, the observation's raw_text
  // is updated to the ROI result (with provenance preserved).
  bool roi_hardening_run = false;
};

[[nodiscard]] OcrGenerationResult generate_ocr_observations(
    const OcrGenerationOptions& options,
    const DecodedCanonicalFrames& frame_input,
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json ocr_generation_result_to_json(
    const OcrGenerationResult& result);

}  // namespace svp::vision
