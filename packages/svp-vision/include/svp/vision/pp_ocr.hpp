#pragma once

#include "svp/vision/color_frame_sampling.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {

struct PpOcrDetection {
  std::string text;
  double score = 0.0;
  int bbox_left = 0;
  int bbox_top = 0;
  int bbox_right = 0;
  int bbox_bottom = 0;
};

struct PpOcrFrameResult {
  std::string frame_id;
  std::int64_t timestamp_us = 0;
  int frame_width = 0;
  int frame_height = 0;
  std::vector<PpOcrDetection> detections;
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
  std::string execution_provider = "cpu";
  int det_limit_side_len = 960;
  double det_thresh = 0.3;
  double det_box_thresh = 0.6;
  double det_unclip_ratio = 1.5;
  int rec_image_height = 48;
  int rec_max_width = 320;
  double min_text_score = 0.0;
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

[[nodiscard]] PpOcrFrameResult run_pp_ocr_on_frame(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const ColorRasterFrame& frame);

[[nodiscard]] nlohmann::json pp_ocr_model_info_to_json(
    const PpOcrModelInfo& info);

}  // namespace svp::vision
