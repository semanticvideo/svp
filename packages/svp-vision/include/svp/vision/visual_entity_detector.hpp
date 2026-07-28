#pragma once

#include "svp/models/runtime.hpp"
#include "svp/vision/color_frame_sampling.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::vision {

struct VisualEntityDetection {
  int box_px[4] = {0, 0, 0, 0};
  double confidence = 0.0;
  // Internal detector category used as supporting identity evidence. It is
  // not a user-facing label.
  int category_index = -1;
};

struct VisualEntityDetectorOptions {
  std::string model_id = "model_rfdetr_nano_coco";
  std::string execution_provider = "cpu";
  // Candidate floor. Weak boxes are available to continue an established
  // track, but the tracker applies a separate 0.15 discovery threshold before
  // allowing a detector-only box to create an entity.
  double confidence_threshold = 0.05;
  // Below the discovery threshold, the detector's category guess is too
  // unstable to constrain identity reconciliation.
  double category_evidence_confidence_threshold = 0.15;
  double nms_iou_threshold = 0.45;
  // Same-class nested lower-confidence boxes commonly describe the same
  // object while having modest union IoU. Suppress them when most of the
  // smaller box is contained by an already retained box.
  double nms_containment_threshold = 0.80;
  // RF-DETR can assign different internal categories to duplicate proposals.
  // Suppress only near-identical cross-category boxes so nested distinct
  // objects, such as a controller held by a person, remain available.
  double cross_category_duplicate_iou_threshold = 0.90;
  // Ignore tiny raster noise and near-full-frame scene proposals. These are
  // proposal-quality limits, not user-facing object-size semantics.
  double minimum_area_ratio = 0.005;
  double maximum_area_ratio = 0.90;
  // Bound the per-frame association workload after deterministic NMS.
  std::size_t maximum_detections = 32;
};

struct VisualEntityDetectorDiagnostics {
  std::size_t queries_evaluated = 0;
  std::size_t confidence_filtered = 0;
  std::size_t area_filtered = 0;
  std::size_t duplicate_filtered = 0;
  std::size_t cap_filtered = 0;
  std::size_t detections_emitted = 0;
};

struct VisualEntityDetectorRuntime {
  std::unique_ptr<svp::models::OnnxSession> session;
  VisualEntityDetectorOptions options;
  std::vector<std::string> model_refs;
  nlohmann::json model_identity;
  VisualEntityDetectorDiagnostics diagnostics;
  std::string blocker;
};

[[nodiscard]] VisualEntityDetectorRuntime load_visual_entity_detector(
    const std::filesystem::path& model_cache_root,
    const VisualEntityDetectorOptions& options = {});

[[nodiscard]] std::vector<VisualEntityDetection> detect_visual_entities(
    VisualEntityDetectorRuntime& runtime,
    const ColorRasterFrame& frame);

}  // namespace svp::vision
