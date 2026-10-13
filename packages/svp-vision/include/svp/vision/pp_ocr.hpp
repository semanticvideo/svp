#pragma once

#include "svp/vision/color_frame_sampling.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/reference_processor_model_ids.hpp"
#include "svp/models/runtime.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace svp::vision {

// One text box as the detector returned it, in frame pixels (not clipped to
// the frame).
struct PpOcrTextBox {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  bool operator==(const PpOcrTextBox&) const = default;
};

struct PpOcrDetection {
  std::string text;
  double score = 0.0;
  int bbox_left = 0;
  int bbox_top = 0;
  int bbox_right = 0;
  int bbox_bottom = 0;
  // True when text and score were copied from this box's recognition on an
  // earlier frame instead of recognized on this one (ocr_recognition_reuse.hpp).
  // The box itself was detected on this frame.
  bool reused = false;
};

struct PpOcrFrameResult {
  std::string frame_id;
  std::int64_t timestamp_us = 0;
  int frame_width = 0;
  int frame_height = 0;
  std::vector<PpOcrDetection> detections;
  double preprocess_detection_ms = 0.0;
  double detection_inference_ms = 0.0;
  double detection_postprocess_ms = 0.0;
  double recognition_total_ms = 0.0;
  double frame_total_ms = 0.0;
  // Detected boxes the recognizer ran on, and boxes whose reading was reused
  // from an earlier frame instead.
  std::size_t recognition_attempt_count = 0;
  std::size_t recognition_reused_count = 0;
};

struct PpOcrModelInfo {
  std::string det_model_id;
  std::string rec_model_id;
  std::string det_model_version;
  std::string rec_model_version;
  std::string det_bundle_blake3;
  std::string rec_bundle_blake3;
  std::string license;
  std::string runtime;
  std::string execution_provider;
  bool model_identity_verified = false;
  bool confidence_calibrated = false;
};

struct PpOcrOptions {
  std::filesystem::path model_cache_root;
  std::string detector_model_id = svp::models::kPpOcrV6MediumDetectorModelId;
  std::string recognizer_model_id =
      svp::models::kPpOcrV6MediumRecognizerModelId;
  std::string manifest_filename = "model.svpmodel.json";
  std::string execution_provider = "cpu";
  int det_limit_side_len = 960;
  double det_thresh = 0.3;
  double det_box_thresh = 0.6;
  double det_unclip_ratio = 1.5;
  int rec_image_height = 48;
  int rec_max_width = 3200;
  double min_text_score = 0.0;
  int graph_optimization_level = -1;
  std::string execution_mode;
  // Session threads from the build's ThreadPlan (ocr_detection and
  // ocr_recognition roles).
  svp::models::OrtThreadCounts det_threads;
  int det_graph_optimization_level = -1;
  std::string det_execution_mode;
  svp::models::OrtThreadCounts rec_threads;
  int rec_graph_optimization_level = -1;
  std::string rec_execution_mode;
  int recognition_parallel_workers = 1;
  int recognition_parallel_min_boxes = 16;
};

struct PpOcrSession {
  PpOcrSession();
  ~PpOcrSession();
  PpOcrSession(PpOcrSession&&) noexcept;
  PpOcrSession& operator=(PpOcrSession&&) noexcept;
  PpOcrSession(const PpOcrSession&) = delete;
  PpOcrSession& operator=(const PpOcrSession&) = delete;

  bool available = false;
  std::string blocker;
  PpOcrModelInfo model_info;
  std::vector<PpOcrFrameResult> results;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] PpOcrSession create_pp_ocr_session(
    const PpOcrOptions& options);

// Runs the recognizer on `boxes` of the frame being read: one detection per
// box, in box order, with empty text where nothing was read.
using PpOcrRecognizer =
    std::function<std::vector<PpOcrDetection>(std::span<const PpOcrTextBox> boxes)>;

// Reads the boxes detected on `frame`: returns one detection per box, in box
// order, running `recognize` on the boxes it does not read another way
// (OcrRecognitionReuse reuses unchanged boxes' earlier readings).
using PpOcrBoxReader = std::function<std::vector<PpOcrDetection>(
    const ColorRasterFrame& frame,
    std::span<const PpOcrTextBox> boxes,
    const PpOcrRecognizer& recognize)>;

// Detection, then a reading for every detected box: by `read_boxes` when
// given (called once, after detection succeeded, even with no boxes),
// otherwise by recognizing every box. Readings with empty text or a score
// below options.min_text_score are dropped either way. Throws
// std::logic_error when `read_boxes` returns the wrong number of readings.
[[nodiscard]] PpOcrFrameResult run_pp_ocr_on_frame(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const ColorRasterFrame& frame,
    const PpOcrBoxReader& read_boxes = {});

[[nodiscard]] PpOcrDetection run_pp_ocr_recognition_on_crop(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const ColorRasterFrame& crop);

[[nodiscard]] nlohmann::json pp_ocr_model_info_to_json(
    const PpOcrModelInfo& info);

}  // namespace svp::vision
