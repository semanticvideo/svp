#pragma once

#include "svp/vision/pp_ocr.hpp"

#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision::pp_ocr_internal {

struct DetInputImage {
  int width = 0;
  int height = 0;
  std::vector<float> data;
  double ratio = 1.0;
};

struct DetBox {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct RecInput {
  std::vector<float> data;
  int width = 0;
  int height = 0;
};

struct ModelBundlePaths {
  std::filesystem::path det_bundle_dir;
  std::filesystem::path rec_bundle_dir;
  std::filesystem::path det_onnx;
  std::filesystem::path rec_onnx;
  std::filesystem::path det_yml;
  std::filesystem::path rec_yml;
  std::string det_model_id;
  std::string rec_model_id;
  std::string det_model_version;
  std::string rec_model_version;
  std::string det_bundle_blake3;
  std::string rec_bundle_blake3;
  std::string license;
  bool det_manifest_loaded = false;
  bool rec_manifest_loaded = false;
  std::optional<svp::models::ModelBundleManifest> det_manifest;
  std::optional<svp::models::ModelBundleManifest> rec_manifest;
};

}  // namespace svp::vision::pp_ocr_internal

namespace svp::vision {

struct PpOcrSession::Impl {
  svp::models::OnnxSession det_session;
  svp::models::OnnxSession rec_session;
  std::vector<std::string> char_dict;
  std::string det_input_name;
  std::string rec_input_name;
};

}  // namespace svp::vision

namespace svp::vision::pp_ocr_internal {

[[nodiscard]] DetInputImage preprocess_detection(
    const ColorRasterFrame& frame,
    int limit_side_len);

[[nodiscard]] std::vector<DetBox> db_postprocess(
    const float* pred_data,
    int pred_h,
    int pred_w,
    double ratio,
    double thresh,
    double box_thresh,
    double unclip_ratio);

[[nodiscard]] RecInput preprocess_recognition(
    const ColorRasterFrame& frame,
    const DetBox& box,
    int target_height,
    int max_width);

[[nodiscard]] std::string ctc_decode(
    const float* pred_data,
    int timesteps,
    int num_classes,
    const std::vector<std::string>& char_dict);

[[nodiscard]] double recognition_confidence(
    const float* pred_data,
    int timesteps,
    int num_classes);

[[nodiscard]] std::vector<std::string> load_char_dict_from_yaml(
    const std::filesystem::path& yml_path);

[[nodiscard]] bool verify_file_hash(
    const svp::models::ModelBundleManifest& manifest,
    const std::filesystem::path& bundle_dir,
    const std::string& filename);

[[nodiscard]] std::optional<ModelBundlePaths> find_pp_ocr_bundles(
    const PpOcrOptions& options);

}  // namespace svp::vision::pp_ocr_internal
