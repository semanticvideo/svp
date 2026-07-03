#pragma once

#include "svp/vision/ocr_generation.hpp"

#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svp::vision::ocr_generation_internal {

struct ParsedNumber {
  std::string raw_text;
  std::string normalized_text;
  std::string number_kind;
  std::string numeric_value;
  std::string unit;
  double confidence = 0.0;
};

struct FrameDetection {
  std::string frame_id;
  std::int64_t timestamp_us = 0;
  std::size_t frame_index = 0;
  int frame_width = 0;
  int frame_height = 0;
  std::string raw_text;
  double confidence = 0.0;
  int bbox_left = 0;
  int bbox_top = 0;
  int bbox_right = 0;
  int bbox_bottom = 0;
};

struct ReconciledObservation {
  std::string raw_text;
  std::string normalized_text;
  double confidence = 0.0;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::size_t frame_start = 0;
  std::size_t frame_end = 0;
  std::vector<std::string> source_frame_ids;
  int bbox_left = 0;
  int bbox_top = 0;
  int bbox_right = 0;
  int bbox_bottom = 0;
  int frame_width = 0;
  int frame_height = 0;
  int detection_count = 1;
};

struct RoiHardeningSummary {
  int improved_observation_count = 0;
  int verified_crop_count = 0;
};

[[nodiscard]] nlohmann::json make_ocr_processor_provenance(
    const std::string& id,
    const std::string& type,
    const std::string& version,
    const std::string& runtime,
    const std::string& status,
    const std::string& note);

[[nodiscard]] std::string normalize_text(const std::string& raw);
[[nodiscard]] std::string alphanumeric_key(const std::string& text);
[[nodiscard]] std::string pad_id(const std::string& prefix,
                                  int index,
                                  int width = 6);
[[nodiscard]] bool roi_text_is_better(const std::string& current_text,
                                       const std::string& roi_text);

[[nodiscard]] std::vector<ParsedNumber> parse_numeric_values(
    const std::string& raw_text,
    double confidence);

[[nodiscard]] std::vector<ReconciledObservation> reconcile_detections(
    const std::vector<FrameDetection>& detections,
    int total_frames,
    double min_confidence = 0.0,
    std::size_t min_text_chars = 3);

void emit_reconciled_records(
    const OcrGenerationOptions& options,
    const std::vector<ReconciledObservation>& reconciled,
    OcrGenerationResult& result);

void refresh_numeric_values_from_observations(OcrGenerationResult& result);

RoiHardeningSummary generate_and_harden_evidence_crops(
    const OcrGenerationOptions& options,
    const std::vector<ReconciledObservation>& reconciled,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir,
    OcrGenerationResult& result);

void write_failure_stage_files(
    const std::filesystem::path& staging_dir,
    const TextAbsenceRecord& text_absence);

bool write_success_stage_files(
    const std::filesystem::path& staging_dir,
    OcrGenerationResult& result);

}  // namespace svp::vision::ocr_generation_internal
