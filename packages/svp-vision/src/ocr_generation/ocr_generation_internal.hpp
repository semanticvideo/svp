#pragma once

#include "svp/vision/ocr_generation.hpp"

#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

// Package frame identity of one decoded OCR sample.
struct OcrSampleFrame {
  std::string frame_id;
  std::size_t frame_index = 0;
};

// What the per-frame OCR loop accumulates for the stage: detections for
// reconciliation, per-frame diagnostics for provenance, and failure state.
struct CollectedOcrFrames {
  std::vector<FrameDetection> detections;
  std::vector<nlohmann::json> frame_diagnostics;
  bool any_frame_failed = false;
  std::string failure_reason_details;
  // Frames OCR looked at (every sample except decode misses), and the size
  // of the first of them.
  int processed_frame_count = 0;
  int processed_frame_width = 0;
  int processed_frame_height = 0;
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

// Gives each decoded sample (status != decode_missed) its package frame, in
// sample order, exactly as the streaming decoder did when it decoded the
// samples itself: registered in `frame_catalog` under kOcrFramePurpose with
// the first decoded sample as keyframe, or numbered frame_000001... by decode
// order when there is no catalog. Entry i is nullopt for a decode miss.
[[nodiscard]] std::vector<std::optional<OcrSampleFrame>> register_ocr_sample_frames(
    const std::vector<OcrSampleDetections>& samples,
    FrameCatalog* frame_catalog);

// Decode outcome of sample records in the shape of the frame decoders'
// result, so OCR reports a failed decode with the decoders' wording: the
// first miss in sample order becomes skipped_reason.
[[nodiscard]] DecodedCanonicalFrames summarize_ocr_sample_decoding(
    const std::vector<OcrSampleDetections>& samples);

// Folds a sample record, in sample order, into the stage's detections and
// diagnostics. Call once per sample that is not a decode miss, with the
// frame register_ocr_sample_frames gave it.
void collect_ocr_sample(const OcrSampleDetections& sample,
                        const OcrSampleFrame& frame,
                        CollectedOcrFrames& collected);

// The stage after per-frame OCR: reports undecodable input or failed frames,
// otherwise reconciles detections, emits records, hardens evidence crops, and
// writes the staged text files and processor provenance. `result` carries the
// availability flags and temporal sampling set so far; `decode_status`
// explains a run in which no frame could be processed.
[[nodiscard]] OcrGenerationResult complete_ocr_generation(
    OcrGenerationResult result,
    const OcrGenerationOptions& options,
    const CollectedOcrFrames& collected,
    const DecodedCanonicalFrames& decode_status,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir);

// Stage result when OCR could not run: text_absence says processor_failed,
// all three processors are not_executed, and the failure stage files are
// written.
[[nodiscard]] OcrGenerationResult finish_ocr_not_executed(
    OcrGenerationResult result,
    const std::filesystem::path& staging_dir,
    const std::string& detector_note,
    const std::string& recognizer_note);

// Blocker text for frame input that yielded nothing to OCR.
[[nodiscard]] std::string ocr_frame_input_blocker(
    const DecodedCanonicalFrames& frames);

void write_failure_stage_files(
    const std::filesystem::path& staging_dir,
    const TextAbsenceRecord& text_absence);

bool write_success_stage_files(
    const std::filesystem::path& staging_dir,
    OcrGenerationResult& result);

}  // namespace svp::vision::ocr_generation_internal
