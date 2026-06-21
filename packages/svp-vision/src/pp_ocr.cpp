#include "svp/vision/pp_ocr.hpp"

#include "svp/core/hash_string.hpp"
#include "svp/models/hash.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace svp::vision {
namespace {

// --- Detection preprocessing (bilinear resize, BGR, ImageNet normalize) ---
struct DetInputImage {
  int width = 0;
  int height = 0;
  std::vector<float> data;  // CHW float, BGR
  double ratio = 1.0;
};

DetInputImage preprocess_detection(
    const ColorRasterFrame& frame,
    int limit_side_len) {
  const int src_w = frame.width;
  const int src_h = frame.height;
  double ratio = 1.0;
  if (std::max(src_h, src_w) > limit_side_len) {
    if (src_h > src_w) {
      ratio = static_cast<double>(limit_side_len) / src_h;
    } else {
      ratio = static_cast<double>(limit_side_len) / src_w;
    }
  }
  int resized_h = static_cast<int>(src_h * ratio);
  int resized_w = static_cast<int>(src_w * ratio);
  resized_h = ((resized_h + 31) / 32) * 32;
  resized_w = ((resized_w + 31) / 32) * 32;
  if (resized_h < 32) resized_h = 32;
  if (resized_w < 32) resized_w = 32;

  DetInputImage img;
  img.width = resized_w;
  img.height = resized_h;
  img.ratio = ratio;
  img.data.resize(static_cast<std::size_t>(3) * resized_h * resized_w);

  const float mean[] = {0.485f, 0.456f, 0.406f};
  const float std_val[] = {0.229f, 0.224f, 0.225f};

  for (int c = 0; c < 3; ++c) {
    const int src_c = (c == 0) ? 2 : (c == 1) ? 1 : 0;
    for (int y = 0; y < resized_h; ++y) {
      const double src_y = static_cast<double>(y) / ratio;
      const int y0 = static_cast<int>(std::min(src_y, static_cast<double>(src_h - 1)));
      const int y1c = std::min(y0 + 1, src_h - 1);
      const double fy = src_y - y0;
      for (int x = 0; x < resized_w; ++x) {
        const double src_x = static_cast<double>(x) / ratio;
        const int x0 = static_cast<int>(std::min(src_x, static_cast<double>(src_w - 1)));
        const int x1c = std::min(x0 + 1, src_w - 1);
        const double fx = src_x - x0;

        const auto& p00 = frame.pixels[y0 * src_w + x0];
        const auto& p01 = frame.pixels[y0 * src_w + x1c];
        const auto& p10 = frame.pixels[y1c * src_w + x0];
        const auto& p11 = frame.pixels[y1c * src_w + x1c];

        const uint8_t v00 = (src_c == 0) ? p00.r : (src_c == 1) ? p00.g : p00.b;
        const uint8_t v01 = (src_c == 0) ? p01.r : (src_c == 1) ? p01.g : p01.b;
        const uint8_t v10 = (src_c == 0) ? p10.r : (src_c == 1) ? p10.g : p10.b;
        const uint8_t v11 = (src_c == 0) ? p11.r : (src_c == 1) ? p11.g : p11.b;

        double val = (1.0 - fx) * (1.0 - fy) * v00 +
                     fx * (1.0 - fy) * v01 +
                     (1.0 - fx) * fy * v10 +
                     fx * fy * v11;
        val = val / 255.0;
        val = (val - mean[c]) / std_val[c];

        img.data[c * resized_h * resized_w + y * resized_w + x] =
            static_cast<float>(val);
      }
    }
  }
  return img;
}

// --- DB post-processing (connected components + box expansion) ---
struct DetBox {
  int left, top, right, bottom;
};

std::vector<DetBox> db_postprocess(
    const float* pred_data,
    int pred_h,
    int pred_w,
    double ratio,
    double thresh,
    double box_thresh,
    double unclip_ratio) {
  std::vector<uint8_t> mask(pred_h * pred_w, 0);
  for (int i = 0; i < pred_h * pred_w; ++i) {
    mask[i] = (pred_data[i] > thresh) ? 1 : 0;
  }

  std::vector<int> labels(pred_h * pred_w, 0);
  int num_labels = 0;
  std::vector<DetBox> boxes;

  const int dx[] = {-1, 1, 0, 0, -1, -1, 1, 1};
  const int dy[] = {0, 0, -1, 1, -1, 1, -1, 1};

  for (int y = 0; y < pred_h; ++y) {
    for (int x = 0; x < pred_w; ++x) {
      const int idx = y * pred_w + x;
      if (mask[idx] == 0 || labels[idx] != 0) continue;

      ++num_labels;
      int min_x = x, min_y = y, max_x = x, max_y = y;
      double sum_score = 0.0;
      int count = 0;

      std::vector<std::pair<int, int>> stack;
      stack.push_back({x, y});
      labels[idx] = num_labels;

      while (!stack.empty()) {
        auto [cx, cy] = stack.back();
        stack.pop_back();

        min_x = std::min(min_x, cx);
        min_y = std::min(min_y, cy);
        max_x = std::max(max_x, cx);
        max_y = std::max(max_y, cy);

        sum_score += pred_data[cy * pred_w + cx];
        ++count;

        for (int d = 0; d < 8; ++d) {
          const int nx = cx + dx[d];
          const int ny = cy + dy[d];
          if (nx < 0 || nx >= pred_w || ny < 0 || ny >= pred_h) continue;
          const int nidx = ny * pred_w + nx;
          if (mask[nidx] == 0 || labels[nidx] != 0) continue;
          labels[nidx] = num_labels;
          stack.push_back({nx, ny});
        }
      }

      if (count > 0) {
        double avg_score = sum_score / count;
        if (avg_score < box_thresh) continue;
      }

      const int bw = max_x - min_x + 1;
      const int bh = max_y - min_y + 1;
      if (bw < 3 || bh < 3) continue;

      const int expand_w = static_cast<int>(bw * (unclip_ratio - 1.0) / 2.0);
      const int expand_h = static_cast<int>(bh * (unclip_ratio - 1.0) / 2.0);

      DetBox box;
      box.left = static_cast<int>((min_x - expand_w) / ratio);
      box.top = static_cast<int>((min_y - expand_h) / ratio);
      box.right = static_cast<int>((max_x + expand_w) / ratio);
      box.bottom = static_cast<int>((max_y + expand_h) / ratio);
      boxes.push_back(box);
    }
  }

  return boxes;
}

// --- Recognition preprocessing (nearest neighbor resize, BGR, /255) ---
struct RecInput {
  std::vector<float> data;
  int width = 0;
  int height = 0;
};

RecInput preprocess_recognition(
    const ColorRasterFrame& frame,
    const DetBox& box,
    int target_height,
    int max_width) {
  int x1 = std::max(0, box.left);
  int y1 = std::max(0, box.top);
  int x2 = std::min(frame.width, box.right);
  int y2 = std::min(frame.height, box.bottom);
  if (x2 <= x1 || y2 <= y1) return {};

  const int crop_w = x2 - x1;
  const int crop_h = y2 - y1;

  double scale = static_cast<double>(target_height) / crop_h;
  int resized_w = std::min(static_cast<int>(crop_w * scale), max_width);
  if (resized_w < 1) resized_w = 1;

  RecInput input;
  input.height = target_height;
  input.width = max_width;
  input.data.resize(static_cast<std::size_t>(3) * target_height * max_width, 0.0f);

  for (int c = 0; c < 3; ++c) {
    const int src_c = (c == 0) ? 2 : (c == 1) ? 1 : 0;
    for (int y = 0; y < target_height; ++y) {
      const int src_y = std::min(static_cast<int>(y / scale), crop_h - 1);
      for (int x = 0; x < resized_w; ++x) {
        const int src_x = std::min(static_cast<int>(x / scale), crop_w - 1);
        const auto& px = frame.pixels[(y1 + src_y) * frame.width + x1 + src_x];
        const uint8_t val = (src_c == 0) ? px.r : (src_c == 1) ? px.g : px.b;
        input.data[c * target_height * max_width + y * max_width + x] =
            static_cast<float>(val) / 255.0f;
      }
    }
  }

  return input;
}

// --- CTC greedy decode ---
std::string ctc_decode(
    const float* pred_data,
    int timesteps,
    int num_classes,
    const std::vector<std::string>& char_dict) {
  std::string result;
  int prev_idx = 0;
  for (int t = 0; t < timesteps; ++t) {
    int max_idx = 0;
    float max_val = pred_data[t * num_classes];
    for (int c = 1; c < num_classes; ++c) {
      const float v = pred_data[t * num_classes + c];
      if (v > max_val) {
        max_val = v;
        max_idx = c;
      }
    }
    if (max_idx != 0 && max_idx != prev_idx) {
      if (max_idx - 1 < static_cast<int>(char_dict.size())) {
        result += char_dict[max_idx - 1];
      }
    }
    prev_idx = max_idx;
  }
  return result;
}

// --- Character dictionary loading from YAML ---
std::vector<std::string> load_char_dict_from_yaml(
    const std::filesystem::path& yml_path) {
  std::vector<std::string> chars;
  std::ifstream in(yml_path);
  if (!in) return chars;

  std::string line;
  bool in_dict_section = false;
  while (std::getline(in, line)) {
    if (line.find("character_dict:") != std::string::npos) {
      in_dict_section = true;
      continue;
    }
    if (in_dict_section) {
      auto trim_start = line.find_first_not_of(" \t");
      if (trim_start == std::string::npos) continue;
      if (line[trim_start] != '-') {
        if (line.find(":") != std::string::npos && trim_start <= 2) break;
        continue;
      }
      std::string item = line.substr(trim_start + 1);
      auto item_start = item.find_first_not_of(" \t");
      if (item_start == std::string::npos) continue;
      item = item.substr(item_start);

      if (item.size() >= 2 && item.front() == '\'' && item.back() == '\'') {
        item = item.substr(1, item.size() - 2);
      } else if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
        item = item.substr(1, item.size() - 2);
      }

      if (item == "\\n") item = "\n";
      else if (item == "\\t") item = "\t";
      else if (item == "\\\\") item = "\\";

      chars.push_back(item);
    }
  }
  return chars;
}

// --- Find model bundle directories ---
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

// Verify that a file's BLAKE3 hash matches the manifest entry for that file.
// Returns true if the file is found in the manifest and its hash matches.
bool verify_file_hash(
    const svp::models::ModelBundleManifest& manifest,
    const std::filesystem::path& bundle_dir,
    const std::string& filename) {
  for (const auto& mf : manifest.files) {
    if (mf.path == filename) {
      const auto file_path = bundle_dir / filename;
      if (!std::filesystem::exists(file_path)) return false;
      std::string computed;
      try {
        computed = svp::models::blake3_hex_for_file(file_path);
      } catch (...) {
        return false;
      }
      return computed == mf.blake3.hex_value();
    }
  }
  return false;
}

std::optional<ModelBundlePaths> find_pp_ocr_bundles(
    const std::filesystem::path& cache_root) {
  ModelBundlePaths paths;

  const std::vector<std::string> det_dir_names = {
    "model_pp_ocrv6_medium_det", "pp_ocrv6_medium_det",
    "model_ppocrv6_medium_det", "ppocrv6_medium_det",
  };
  const std::vector<std::string> rec_dir_names = {
    "model_pp_ocrv6_medium_rec", "pp_ocrv6_medium_rec",
    "model_ppocrv6_medium_rec", "ppocrv6_medium_rec",
  };

  for (const auto& name : det_dir_names) {
    auto dir = cache_root / name;
    if (std::filesystem::exists(dir)) {
      paths.det_bundle_dir = dir;
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto p = entry.path();
        if (p.extension() == ".onnx") paths.det_onnx = p;
        if (p.extension() == ".yml" || p.extension() == ".yaml") paths.det_yml = p;
      }
      auto manifest_path = dir / "model_manifest.json";
      if (std::filesystem::exists(manifest_path)) {
        try {
          auto manifest = svp::models::load_model_bundle_manifest(manifest_path);
          paths.det_model_id = manifest.model_id;
          paths.det_model_version = manifest.model_version;
          paths.det_bundle_blake3 = manifest.bundle_blake3.hex_value();
          if (!manifest.license.empty()) paths.license = manifest.license;
          paths.det_manifest = std::move(manifest);
          paths.det_manifest_loaded = true;
        } catch (...) {}
      }
      if (paths.det_model_id.empty()) paths.det_model_id = name;
      break;
    }
  }

  for (const auto& name : rec_dir_names) {
    auto dir = cache_root / name;
    if (std::filesystem::exists(dir)) {
      paths.rec_bundle_dir = dir;
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto p = entry.path();
        if (p.extension() == ".onnx") paths.rec_onnx = p;
        if (p.extension() == ".yml" || p.extension() == ".yaml") paths.rec_yml = p;
      }
      auto manifest_path = dir / "model_manifest.json";
      if (std::filesystem::exists(manifest_path)) {
        try {
          auto manifest = svp::models::load_model_bundle_manifest(manifest_path);
          paths.rec_model_id = manifest.model_id;
          paths.rec_model_version = manifest.model_version;
          paths.rec_bundle_blake3 = manifest.bundle_blake3.hex_value();
          if (!manifest.license.empty() && paths.license.empty())
            paths.license = manifest.license;
          paths.rec_manifest = std::move(manifest);
          paths.rec_manifest_loaded = true;
        } catch (...) {}
      }
      if (paths.rec_model_id.empty()) paths.rec_model_id = name;
      break;
    }
  }

  if (paths.det_onnx.empty() || paths.rec_onnx.empty()) return std::nullopt;
  if (paths.license.empty()) paths.license = "Apache-2.0";
  return paths;
}

}  // namespace

// --- PpOcrSession::Impl ---

struct PpOcrSession::Impl {
  svp::models::OnnxSession det_session;
  svp::models::OnnxSession rec_session;
  std::vector<std::string> char_dict;
  std::string det_input_name;
  std::string rec_input_name;
};

PpOcrSession::PpOcrSession() : impl_(std::make_unique<Impl>()) {}
PpOcrSession::~PpOcrSession() = default;
PpOcrSession::PpOcrSession(PpOcrSession&&) noexcept = default;
PpOcrSession& PpOcrSession::operator=(PpOcrSession&&) noexcept = default;

PpOcrSession create_pp_ocr_session(const PpOcrOptions& options) {
  PpOcrSession session;

  if (!svp::models::OnnxSession::is_available()) {
    session.blocker = "ONNX Runtime is not available";
    return session;
  }

  auto bundles = find_pp_ocr_bundles(options.model_cache_root);
  if (!bundles) {
    session.blocker = "PP-OCR model bundles not found in model cache: " +
                      options.model_cache_root.string();
    return session;
  }

  // Verify model file hashes against manifests before loading sessions.
  // If manifests are missing or hashes don't match, PP-OCR cannot claim
  // trusted model identity. We block the session entirely because trusted
  // OCR requires verified model identity.
  bool identity_verified = false;
  if (bundles->det_manifest_loaded && bundles->rec_manifest_loaded &&
      bundles->det_manifest && bundles->rec_manifest) {
    const auto& dm = *bundles->det_manifest;
    const auto& rm = *bundles->rec_manifest;

    bool det_ok = verify_file_hash(
        dm, bundles->det_bundle_dir, bundles->det_onnx.filename().string());
    bool rec_onnx_ok = verify_file_hash(
        rm, bundles->rec_bundle_dir, bundles->rec_onnx.filename().string());
    bool rec_yml_ok = !bundles->rec_yml.empty() &&
        verify_file_hash(
            rm, bundles->rec_bundle_dir, bundles->rec_yml.filename().string());

    if (det_ok && rec_onnx_ok && rec_yml_ok) {
      identity_verified = true;
    } else {
      session.blocker =
          "PP-OCR model file hash verification failed: "
          "det_onnx=" + std::string(det_ok ? "ok" : "FAIL") +
          ", rec_onnx=" + std::string(rec_onnx_ok ? "ok" : "FAIL") +
          ", rec_yml=" + std::string(rec_yml_ok ? "ok" : "FAIL");
      return session;
    }
  } else {
    session.blocker =
        "PP-OCR model manifests missing or malformed; "
        "trusted OCR requires verified model identity. "
        "det_manifest=" + std::string(bundles->det_manifest_loaded ? "loaded" : "missing") +
        ", rec_manifest=" + std::string(bundles->rec_manifest_loaded ? "loaded" : "missing");
    return session;
  }

  if (!bundles->rec_yml.empty() && std::filesystem::exists(bundles->rec_yml)) {
    session.impl_->char_dict = load_char_dict_from_yaml(bundles->rec_yml);
  }
  if (session.impl_->char_dict.empty()) {
    session.blocker = "Failed to load character dictionary from: " +
                      bundles->rec_yml.string();
    return session;
  }

  svp::models::ModelBundleManifest det_manifest{
      "", "", bundles->det_model_id, bundles->det_model_version,
      svp::core::HashString{svp::core::HashAlgorithm::blake3, ""},
      std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      "onnxruntime", "onnx", "",
      {}, {}, {}, {}, {}};
  svp::models::ModelBundleFile det_file{
      bundles->det_onnx.filename().string(), "onnx",
      svp::core::HashString{svp::core::HashAlgorithm::blake3, ""}};
  det_manifest.files.push_back(det_file);

  svp::models::OnnxSessionOptions sess_opts;
  sess_opts.execution_provider = options.execution_provider;

  try {
    session.impl_->det_session = svp::models::OnnxSession::load(
        det_manifest, bundles->det_bundle_dir, sess_opts);
  } catch (const std::exception& e) {
    session.blocker = std::string("Failed to load PP-OCR detector: ") + e.what();
    return session;
  }

  svp::models::ModelBundleManifest rec_manifest{
      "", "", bundles->rec_model_id, bundles->rec_model_version,
      svp::core::HashString{svp::core::HashAlgorithm::blake3, ""},
      std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      "onnxruntime", "onnx", "",
      {}, {}, {}, {}, {}};
  svp::models::ModelBundleFile rec_file{
      bundles->rec_onnx.filename().string(), "onnx",
      svp::core::HashString{svp::core::HashAlgorithm::blake3, ""}};
  rec_manifest.files.push_back(rec_file);

  try {
    session.impl_->rec_session = svp::models::OnnxSession::load(
        rec_manifest, bundles->rec_bundle_dir, sess_opts);
  } catch (const std::exception& e) {
    session.blocker = std::string("Failed to load PP-OCR recognizer: ") + e.what();
    return session;
  }

  auto det_io = session.impl_->det_session.io_spec();
  if (!det_io.inputs.empty()) {
    session.impl_->det_input_name = det_io.inputs[0].name;
  }
  auto rec_io = session.impl_->rec_session.io_spec();
  if (!rec_io.inputs.empty()) {
    session.impl_->rec_input_name = rec_io.inputs[0].name;
  }

  session.available = true;
  session.model_info.det_model_id = bundles->det_model_id;
  session.model_info.rec_model_id = bundles->rec_model_id;
  session.model_info.det_model_version = bundles->det_model_version;
  session.model_info.rec_model_version = bundles->rec_model_version;
  session.model_info.det_bundle_blake3 = bundles->det_bundle_blake3;
  session.model_info.rec_bundle_blake3 = bundles->rec_bundle_blake3;
  session.model_info.license = bundles->license;
  session.model_info.runtime = "onnxruntime";
  session.model_info.execution_provider = options.execution_provider;
  session.model_info.model_identity_verified = identity_verified;
  session.model_info.confidence_calibrated = false;

  return session;
}

PpOcrFrameResult run_pp_ocr_on_frame(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const ColorRasterFrame& frame) {
  PpOcrFrameResult result;
  result.frame_id = frame.frame_id;
  result.timestamp_us = frame.timestamp_us;
  result.frame_width = frame.width;
  result.frame_height = frame.height;

  if (!session.available || frame.pixels.empty()) return result;

  // Phase 1: Detection
  DetInputImage det_input = preprocess_detection(frame, options.det_limit_side_len);

  std::vector<std::int64_t> det_shape = {1, 3, det_input.height, det_input.width};
  std::vector<float> det_output;
  std::vector<std::int64_t> det_output_shape;
  try {
    auto [data, shape] = session.impl_->det_session.run_raw_with_shape(
        session.impl_->det_input_name,
        det_input.data.data(),
        det_input.data.size(),
        det_shape);
    det_output = std::move(data);
    det_output_shape = std::move(shape);
  } catch (const std::exception&) {
    return result;
  }

  if (det_output.empty()) return result;

  // Determine output dimensions from the actual runtime output shape
  const int total_pixels = static_cast<int>(det_output.size());
  int pred_h = det_input.height;
  int pred_w = det_input.width;
  if (det_output_shape.size() >= 2) {
    auto sh = det_output_shape[det_output_shape.size() - 2];
    auto sw = det_output_shape[det_output_shape.size() - 1];
    if (sh > 0 && sw > 0) {
      pred_h = static_cast<int>(sh);
      pred_w = static_cast<int>(sw);
    }
  }
  if (pred_h * pred_w != total_pixels) {
    pred_h = det_input.height / 4;
    pred_w = det_input.width / 4;
    if (pred_h * pred_w != total_pixels) {
      pred_h = static_cast<int>(std::sqrt(static_cast<double>(total_pixels)));
      pred_w = total_pixels / pred_h;
    }
  }

  // The DB postprocess divides box coordinates by `ratio` to map from
  // the resized detection image back to original frame coordinates.
  // pred_w/pred_h are the model output dimensions (typically 1/4 of the
  // detection input), but the ratio we need is just the preprocessing
  // resize ratio (original -> detection input size).
  const double ratio = det_input.ratio;

  auto boxes = db_postprocess(
      det_output.data(), pred_h, pred_w,
      ratio, options.det_thresh, options.det_box_thresh,
      options.det_unclip_ratio);

  // Phase 2: Recognition
  for (const auto& box : boxes) {
    auto rec_input = preprocess_recognition(
        frame, box, options.rec_image_height, options.rec_max_width);
    if (rec_input.data.empty()) continue;

    std::vector<std::int64_t> rec_shape = {1, 3, rec_input.height, rec_input.width};
    std::vector<float> rec_output;
    std::vector<std::int64_t> rec_output_shape;
    try {
      auto [rdata, rshape] = session.impl_->rec_session.run_raw_with_shape(
          session.impl_->rec_input_name,
          rec_input.data.data(),
          rec_input.data.size(),
          rec_shape);
      rec_output = std::move(rdata);
      rec_output_shape = std::move(rshape);
    } catch (const std::exception&) {
      continue;
    }

    if (rec_output.empty()) continue;

    // Derive num_classes and timesteps from the actual output shape.
    // PP-OCR recognizer outputs [1, T, C] where C = dict_size + 1 (blank)
    // but some models include extra tokens, so use the actual shape.
    int rec_num_classes = static_cast<int>(session.impl_->char_dict.size()) + 1;
    int rec_timesteps = static_cast<int>(rec_output.size()) / rec_num_classes;
    if (rec_output_shape.size() >= 3) {
      auto sh_t = rec_output_shape[rec_output_shape.size() - 2];
      auto sh_c = rec_output_shape[rec_output_shape.size() - 1];
      if (sh_t > 0 && sh_c > 0) {
        rec_timesteps = static_cast<int>(sh_t);
        rec_num_classes = static_cast<int>(sh_c);
      }
    }
    if (rec_timesteps <= 0 || rec_num_classes <= 0) continue;

    std::string text = ctc_decode(
        rec_output.data(), rec_timesteps, rec_num_classes,
        session.impl_->char_dict);

    if (text.empty()) continue;

    // Compute confidence as mean softmax probability of non-blank tokens.
    // PP-OCRv6 recognizer outputs raw logits (not softmaxed), so softmax
    // over 18710 classes produces very small probabilities. This is an
    // honest representation of the model's confidence — it is uncalibrated
    // and should not be treated as a reliable quality signal.
    double conf = 0.0;
    int non_blank_count = 0;
    for (int t = 0; t < rec_timesteps; ++t) {
      const float* row = rec_output.data() + t * rec_num_classes;
      int max_idx = 0;
      float max_val = row[0];
      for (int c = 1; c < rec_num_classes; ++c) {
        if (row[c] > max_val) {
          max_val = row[c];
          max_idx = c;
        }
      }
      if (max_idx != 0) {
        float sum_exp = 0.0f;
        for (int c = 0; c < rec_num_classes; ++c) {
          sum_exp += std::exp(row[c] - max_val);
        }
        conf += 1.0f / sum_exp;
        ++non_blank_count;
      }
    }
    if (non_blank_count > 0) conf /= non_blank_count;

    PpOcrDetection det;
    det.text = text;
    det.score = conf;
    det.bbox_left = std::max(0, box.left);
    det.bbox_top = std::max(0, box.top);
    det.bbox_right = std::min(frame.width, box.right);
    det.bbox_bottom = std::min(frame.height, box.bottom);

    if (det.score >= options.min_text_score) {
      result.detections.push_back(std::move(det));
    }
  }

  return result;
}

nlohmann::json pp_ocr_model_info_to_json(const PpOcrModelInfo& info) {
  return {
    {"det_model_id", info.det_model_id},
    {"rec_model_id", info.rec_model_id},
    {"det_model_version", info.det_model_version},
    {"rec_model_version", info.rec_model_version},
    {"det_bundle_blake3", info.det_bundle_blake3},
    {"rec_bundle_blake3", info.rec_bundle_blake3},
    {"license", info.license},
    {"runtime", info.runtime},
    {"execution_provider", info.execution_provider},
    {"model_identity_verified", info.model_identity_verified},
    {"confidence_calibrated", info.confidence_calibrated},
  };
}

}  // namespace svp::vision
