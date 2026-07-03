#pragma once

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/ocr_temporal_sampling.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace svp::media { struct MediaIngestPlan; }

namespace svp::vision {

struct OcrGenerationOptions {
  std::filesystem::path model_cache_root;
  std::filesystem::path ffmpeg_path = "ffmpeg";
  std::string language = "eng";

  // Soft target for total text observations.  Instead of a hard cap that
  // silently drops observations from later frames, the reconciliation step
  // will report observation_count_capped when this target is exceeded.
  // Set to 0 to disable the cap entirely.
  std::size_t target_max_observations = 0;

  const svp::media::MediaIngestPlan* media_plan = nullptr;
  int ocr_frame_width = 0;
  int ocr_frame_height = 0;
  std::string performance_profile;
  int recognition_parallel_workers = 1;
  int recognition_parallel_min_boxes = 16;
  int canonical_raster_width = 0;
  int canonical_raster_height = 0;
  bool generate_evidence_crops = false;
  std::size_t max_total_crops = 50;
  std::int64_t max_total_crop_bytes = 2 * 1024 * 1024;
  std::string crop_coverage_policy = "one_per_observation";
  int crop_min_jpeg_quality = 50;
  OcrSamplingConfig sampling_config;
  FrameCatalog* frame_catalog = nullptr;
  FrameProgressCallback on_progress;
  EvidenceCropProgressCallback on_evidence_crop_progress;
  EvidenceCropProgressCallback on_evidence_roi_progress;
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
  std::int64_t total_reconciled_observations = 0;
  bool observation_count_capped = false;
  std::size_t target_max_observations = 0;
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
  std::string crop_coverage_policy;
  std::size_t crop_effective_max_total_crops = 0;
  std::int64_t crop_effective_max_total_crop_bytes = 0;
  std::int64_t crops_skipped_by_count_cap = 0;
  std::int64_t crops_skipped_by_byte_cap = 0;
  std::int64_t crops_skipped_by_extraction = 0;
  std::int64_t crop_total_observations_requested = 0;
  bool every_observation_has_crop = false;
  std::string crop_coverage_status;
  // ROI hardening results, parallel to text_observations.
  // When ROI OCR produced better text, the observation's raw_text
  // is updated to the ROI result (with provenance preserved).
  bool roi_hardening_run = false;
  OcrTemporalSamplingResult temporal_sampling;
};

struct OcrSourceFrameDimensions {
  int width = 0;
  int height = 0;
};

[[nodiscard]] OcrSourceFrameDimensions derive_ocr_source_frame_dimensions(
    int stored_width,
    int stored_height,
    int rotation_degrees);

[[nodiscard]] OcrGenerationResult generate_ocr_observations(
    const OcrGenerationOptions& options,
    const DecodedCanonicalFrames& frame_input,
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json ocr_generation_result_to_json(
    const OcrGenerationResult& result);

[[nodiscard]] DecodedCanonicalFrames decode_ocr_frames_temporal(
    const OcrGenerationOptions& options,
    OcrTemporalSamplingResult& out_sampling);

}  // namespace svp::vision
