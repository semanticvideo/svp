#pragma once

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/ocr_batch_policy.hpp"
#include "svp/vision/ocr_frame_detections.hpp"
#include "svp/vision/ocr_sample_plan.hpp"
#include "svp/vision/ocr_temporal_sampling.hpp"
#include "svp/vision/pp_ocr.hpp"
#include "svp/models/thread_plan.hpp"

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
  // From the build's ThreadPlan (ocr_recognition_workers, ocr_detection,
  // ocr_recognition). Environment overrides are applied to the plan, not here.
  int recognition_parallel_workers = 1;
  svp::models::OrtThreadCounts detection_threads;
  svp::models::OrtThreadCounts recognition_threads;
  int recognition_parallel_min_boxes = 16;
  int canonical_raster_width = 0;
  int canonical_raster_height = 0;
  bool generate_evidence_crops = false;
  std::size_t max_total_crops = 50;
  std::int64_t max_total_crop_bytes = 2 * 1024 * 1024;
  std::string crop_coverage_policy = "one_per_observation";
  int crop_min_jpeg_quality = 50;
  OcrSamplingConfig sampling_config;
  // How generate_ocr_observations cuts the sample plan into frame batches.
  // Scheduling only: the output is identical for every policy.
  OcrBatchPolicy batch_policy;
  FrameCatalog* frame_catalog = nullptr;
  FrameProgressCallback on_progress;
  EvidenceCropProgressCallback on_evidence_crop_progress;
  EvidenceCropProgressCallback on_evidence_roi_progress;
  // Optional: runs the evidence-crop per-observation work elsewhere
  // (dispatched_work.hpp). Empty in every build that is not --distributed.
  EvidenceCropDispatcher evidence_crop_dispatcher;
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

// Longest side of the frames decoded for OCR text detection. Source display
// dimensions are kept when both sides fit; otherwise the frame is scaled to
// this bound, preserving aspect ratio, to keep text readable while bounding
// per-frame OCR cost.
inline constexpr int kOcrMaxFrameDimension = 1920;

// Frame dimensions used to decode OCR samples for a media plan: the source
// dimensions (swapped for a +/-90 degree rotation) capped at
// kOcrMaxFrameDimension. Shared by every OCR stage entry point and the frame
// plan so they agree on whether OCR decodes its own samples.
[[nodiscard]] OcrSourceFrameDimensions ocr_decode_frame_dimensions(
    const svp::media::MediaIngestPlan& media_plan);

// The OCR stage. With a media plan and decode size it runs the batched
// pipeline in this process: plan_ocr_samples, then run_ocr_frame_batch over
// each batch of options.batch_policy, then the reducer
// (reduce_ocr_frame_batches). Otherwise it runs OCR over `frame_input`, the
// caller's canonical frames.
[[nodiscard]] OcrGenerationResult generate_ocr_observations(
    const OcrGenerationOptions& options,
    const DecodedCanonicalFrames& frame_input,
    const std::filesystem::path& staging_dir);

// Plan step of the batched OCR stage: the sample schedule for the media
// duration (or the SVP_OCR_DIAG_TIMESTAMPS_US override) at the options'
// decode size, computed once before any frame is decoded. nullopt when the
// options carry no media plan or decode size; `samples` may be empty when
// the duration is unknown.
[[nodiscard]] std::optional<OcrSamplePlan> plan_ocr_samples(
    const OcrGenerationOptions& options);

// PP-OCR options for the stage: model cache and thread counts from the
// options (the build's ThreadPlan), execution provider, graph optimization,
// and execution mode from the SVP_OCR_* diagnostic environment overrides.
// Resolved once on the coordinator and sent to frame batch tasks as explicit
// parameters, so a worker never consults its own environment or host.
[[nodiscard]] PpOcrOptions make_ocr_pp_ocr_options(
    const OcrGenerationOptions& options);

// Reduce step of the batched OCR stage (plan §2.4 item 3): assembles frame
// batch results by sample ordinal (any partition, any completion order),
// registers the decoded samples' frames in sample order, then runs the
// unchanged reconciliation, record emission, evidence crops, and staging.
// `pp_ocr_session` (created from `pp_ocr_opts`) serves the evidence-crop ROI
// re-read. Throws OcrFrameBatchReductionError when the batches do not cover
// the plan exactly.
[[nodiscard]] OcrGenerationResult reduce_ocr_frame_batches(
    const OcrGenerationOptions& options,
    const OcrSamplePlan& plan,
    std::vector<std::vector<OcrSampleDetections>> batch_results,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json ocr_generation_result_to_json(
    const OcrGenerationResult& result);

[[nodiscard]] DecodedCanonicalFrames decode_ocr_frames_temporal(
    const OcrGenerationOptions& options,
    OcrTemporalSamplingResult& out_sampling);

}  // namespace svp::vision
