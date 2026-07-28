#pragma once

#include "svp/models/runtime.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/color_frame_sampling.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::vision {

struct VisualEntityEmbeddingRuntime {
  std::unique_ptr<svp::models::OnnxSession> session;
  std::vector<std::string> model_refs;
  std::string limitations_note;
};

struct ExternalEntityProposal {
  std::string frame_id;
  int box_px[4] = {0, 0, 0, 0};
  double confidence = 0.0;
  int detector_category_index = -1;
  std::string source;
};

// All constants in this struct have documented rationale referencing
// spec §20.6 step numbers and OpenCV documentation recommendations.
// No magic numbers. No sample-specific values.
struct VisualEntityTrackerOptions {
  // --- Keyframe selection (§20.6 step 2) ---
  // Base interval: select every Nth frame per shot.
  // Rationale: "Spec §20.6 step 2 says 'every Nth frame based on motion
  //   change'. 5 frames balances temporal resolution vs compute cost for
  //   24-30fps video. The interval adapts downward (more keyframes) when
  //   frame-to-frame pixel difference exceeds the motion change threshold."
  int keyframe_interval_frames = 5;

  // Motion change threshold for adaptive keyframe selection.
  // Rationale: "When mean frame-to-frame pixel difference exceeds this
  //   fraction of the max pixel value (255), the keyframe interval is
  //   halved. 0.02 = ~5 pixel mean difference, a conservative threshold
  //   that detects scene-internal motion without reacting to noise."
  double motion_change_threshold = 0.02;

  // --- Shi-Tomasi corners (§20.6 step 3) ---
  // Rationale: "Spec §20.6 step 3. Sufficient feature density for optical
  //   flow tracking without excessive compute. Values from OpenCV
  //   documentation recommendations for goodFeaturesToTrack."
  int max_corners = 200;
  double corner_quality_level = 0.01;
  double corner_min_distance = 10.0;

  // --- Lucas-Kanade optical flow (§20.6 step 4) ---
  // Rationale: "Spec §20.6 step 4. Standard LK parameters from OpenCV
  //   documentation. 21x21 window with 3 pyramid levels handles moderate
  //   motion. 30 iterations or 0.03 epsilon convergence is standard."
  int lk_window_width = 21;
  int lk_window_height = 21;
  int lk_max_level = 3;
  int lk_max_count = 30;
  double lk_epsilon = 0.03;

  // --- Farneback dense optical flow (§20.6 step 5) ---
  // Rationale: "Spec §20.6 step 5. Standard Farneback parameters from
  //   OpenCV documentation. Window 15, poly_n=5, poly_sigma=1.5 are
  //   recommended defaults for dense flow quality."
  int farneback_window_size = 15;
  int farneback_poly_n = 5;
  double farneback_poly_sigma = 1.5;
  int farneback_iterations = 1;
  int farneback_pyr_scale = 2;  // 1/pyr_scale pyramid scaling

  // --- RANSAC homography (§20.6 step 6) ---
  // Rationale: "Spec §20.6 step 6. 3.0-pixel reprojection error threshold
  //   is standard for homography estimation from optical flow vectors.
  //   Values from OpenCV findHomography documentation."
  double ransac_threshold = 3.0;

  // --- Residual motion clustering (§20.6 step 8) ---
  // Rationale: "Spec §20.6 step 8. Below 2.0 pixels of residual motion
  //   magnitude, a region is considered static background and not
  //   clustered as a candidate moving region."
  double min_motion_magnitude = 2.0;

  // --- Region area filter (§20.6 step 10) ---
  // Rationale: "Spec §20.6 step 10. Regions smaller than 0.5% of frame
  //   area are noise and discarded. This prevents spurious tiny regions
  //   from optical flow jitter."
  double min_region_area_ratio = 0.005;

  // --- GrabCut mask refinement (§20.6 step 10, §5.9) ---
  // Rationale: "Spec §20.6 step 10 and §5.9. 5 iterations balances mask
  //   quality vs compute cost. GrabCut is seeded by region bounding boxes."
  int grabcut_iterations = 5;

  // Detector boxes already provide object-level localization. One GrabCut
  // pass refines their boundary without repeating the expensive iterative
  // search used for unsupported motion boxes.
  int detector_grabcut_iterations = 1;

  // --- Kalman filter (§20.6 step 12) ---
  // Standard constant-velocity Kalman model for 2D box tracking.
  // State = [x, y, w, h, vx, vy, vw, vh], Measurement = [x, y, w, h]
  // Rationale: "Spec §20.6 step 12. Standard constant-velocity Kalman
  //   model. Process noise 1e-4 and measurement noise 1e-1 are standard
  //   for smooth visual tracking where measurements are noisy but
  //   physically plausible."
  double kalman_process_noise = 1e-4;
  double kalman_measurement_noise = 1e-1;

  // --- Track reacquisition (§20.6 step 13) ---
  // Rationale: "Spec §20.6 step 13. 10 frames of tolerance before track
  //   loss. 0.75 cosine similarity threshold for embedding-based
  //   reacquisition — high enough to avoid false matches, low enough
  //   to tolerate moderate appearance change."
  int max_lost_frames = 10;
  double appearance_similarity_threshold = 0.75;

  // A scene cut removes spatial continuity, so appearance must independently
  // satisfy the same strong threshold used by window-level reacquisition.
  // This prevents a depicted person in a graphic from taking over a live
  // person's identity across the cut.
  double scene_cut_similarity_threshold = 0.90;

  // Minimum predicted-box overlap for direct temporal association. Below
  // this value, appearance evidence must independently support the match.
  double association_iou_threshold = 0.1;

  // Detector-only boxes below this confidence may continue a spatially
  // supported track, but cannot independently create a new entity.
  double detector_discovery_confidence_threshold = 0.15;

  // Weak detector evidence may continue a track only when at least half of
  // the predicted and observed boxes overlap. The ordinary 0.1 association
  // threshold is intentionally insufficient for low-confidence boxes.
  double weak_detector_continuation_iou_threshold = 0.50;

  // --- Visual embedding model ---
  std::string embedding_model_id = "model_nomic_embed_vision_v1_5";
  std::string execution_provider = "cpu";
  VisualEntityEmbeddingRuntime* embedding_runtime = nullptr;
  std::vector<ExternalEntityProposal> external_proposals;

  // --- Relationship thresholds (§20.8) ---
  // Rationale: "Spec §20.8. IoU > 0.3 for 'overlaps' relationship.
  //   Centroid distance < 0.15 (normalized) for 'near' relationship.
  //   Containment > 0.7 for 'contains' relationship."
  double overlap_iou_threshold = 0.3;
  double near_centroid_threshold = 0.15;
  double contains_ratio_threshold = 0.7;

  // --- Depth-based candidate detection ---
  // Minimum depth variance (as fraction of mean depth) to consider depth non-flat
  double depth_variance_threshold = 0.05;
  // IoU threshold for merging depth and motion candidates
  double candidate_merge_iou_threshold = 0.3;

  // Progress callbacks for visual tracking and visual embedding inference
  std::function<void(std::size_t current, std::size_t total)> on_tracking_progress;
  std::function<void(std::size_t current, std::size_t total)> on_visual_embedding_progress;
};

[[nodiscard]] VisualEntityEmbeddingRuntime
load_visual_entity_embedding_runtime(
    const std::filesystem::path& model_cache_root,
    const std::string& model_id,
    const std::string& execution_provider);

struct TrackedRegion {
  std::string region_id;
  std::string entity_id;
  std::string track_id;
  std::string frame_id;
  std::int64_t timestamp_us = 0;
  // Normalized bbox [x0, y0, x1, y1] in [0, 1]
  double box_norm[4] = {0, 0, 0, 0};
  // Pixel bbox in canonical raster
  int box_px[4] = {0, 0, 0, 0};
  // Centroid normalized
  double centroid_norm[2] = {0, 0};
  double screen_area_ratio = 0;
  // Depth summary
  double median_inverse_depth = 0;
  double near_percentile_10 = 0;
  double far_percentile_90 = 0;
  // Mask (binary, row-major, width*height bytes)
  std::vector<std::uint8_t> mask_pixels;
  int mask_width = 0;
  int mask_height = 0;
  // Embedding
  std::vector<float> embedding;
  std::string embedding_model_id;
  // Confidence
  double confidence = 0;
  // Mask reference (id of the mask in spatial/masks.index.jsonl)
  std::string mask_ref;
  // Depth reference (id of the depth entry in spatial/depth.index.jsonl)
  std::string depth_ref;
  // Candidate source: "motion", "depth", or "fused_motion_depth"
  std::string candidate_source;
  // Internal detector evidence used for association; not an emitted label.
  int detector_category_index = -1;
};

struct EntityRecord {
  std::string entity_id;
  std::string entity_type;  // "visual_entity", "background_region", "screen_region", "unknown_region"
  std::int64_t first_seen_us = 0;
  std::int64_t last_seen_us = 0;
  std::vector<std::string> track_ids;
  double average_visibility = 0;
  double average_screen_area = 0;
  std::string processor_id;
  std::vector<nlohmann::json> evidence_sources;
};

struct TrackRecord {
  std::string track_id;
  std::string entity_id;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::string start_frame_id;
  std::string end_frame_id;
  int region_count = 0;
  int lost_frame_count = 0;
  bool reacquired = false;
  double confidence = 0;
  std::string processor_id;
  std::string tracking_method;
  // Candidate source: "motion", "depth", or "fused_motion_depth"
  std::string candidate_source;
};

struct EntityTrackResult {
  std::vector<TrackedRegion> regions;
  std::vector<EntityRecord> entities;
  std::vector<TrackRecord> tracks;

  // Provenance
  std::string processor_id;
  std::vector<std::string> model_refs;
  std::string runtime;
  std::string execution_provider;
  std::string confidence_calibration_status;
  std::string processing_status = "completed";
  std::string limitations_note;
  std::string opencv_version;
  nlohmann::json parameters_json;
};

// Run the visual entity tracker pipeline per spec §20.6 steps 1-14.
//
// Input: decoded canonical frames in presentation order, depth data,
// shot boundaries, and model cache root for the visual embedding model.
//
// Output: regions, entities, tracks with masks, embeddings, and provenance.
//
// Degenerate sources (all-constant frames, black video, etc.) produce
// zero entities, zero regions, and empty tracks per spec §13.4.
[[nodiscard]] EntityTrackResult run_visual_entity_tracker(
    const std::vector<ColorRasterFrame>& decoded_frames,
    const std::vector<std::uint16_t>& depth_data,
    const std::vector<std::string>& depth_frame_ids,
    const std::vector<std::pair<std::string, std::int64_t>>& shot_boundaries,
    const std::filesystem::path& model_cache_root,
    const VisualEntityTrackerOptions& options = {});

// Convert a binary mask (row-major, 1=foreground, 0=background) to
// SVP RLE encoding per spec §14.3.
// RLE starts with background run length, runs are unsigned LEB128.
// Scan order is row-major from top-left to bottom-right.
// Decoded run total MUST equal width * height.
[[nodiscard]] std::vector<std::uint8_t> encode_mask_rle(
    const std::uint8_t* mask_pixels,
    int width,
    int height);

// Decode SVP RLE mask back to binary pixels.
// Returns empty vector on error (run total != width * height).
[[nodiscard]] std::vector<std::uint8_t> decode_mask_rle(
    const std::uint8_t* rle_data,
    std::size_t rle_size,
    int width,
    int height);

}  // namespace svp::vision
