#include "svp/vision/pp_ocr.hpp"

#include "pp_ocr/pp_ocr_internal.hpp"

#include "svp/core/hash_string.hpp"
#include "svp/core/memory_diagnostics.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace svp::vision {
namespace {

using pp_ocr_internal::DetBox;
using pp_ocr_internal::DetInputImage;
using pp_ocr_internal::RecInput;
using pp_ocr_internal::ctc_decode;
using pp_ocr_internal::db_postprocess;
using pp_ocr_internal::find_pp_ocr_bundles;
using pp_ocr_internal::load_char_dict_from_yaml;
using pp_ocr_internal::preprocess_detection;
using pp_ocr_internal::preprocess_recognition;
using pp_ocr_internal::recognition_confidence;
using pp_ocr_internal::verify_file_hash;

using SteadyClock = std::chrono::steady_clock;

double elapsed_ms(SteadyClock::time_point start, SteadyClock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

struct RecognitionShape {
  int timesteps = 0;
  int num_classes = 0;
};

RecognitionShape recognition_shape(
    const std::vector<float>& output,
    const std::vector<std::int64_t>& output_shape,
    int char_dict_size) {
  RecognitionShape shape;
  shape.num_classes = char_dict_size + 1;
  shape.timesteps = static_cast<int>(output.size()) / shape.num_classes;
  if (output_shape.size() >= 3) {
    const auto sh_t = output_shape[output_shape.size() - 2];
    const auto sh_c = output_shape[output_shape.size() - 1];
    if (sh_t > 0 && sh_c > 0) {
      shape.timesteps = static_cast<int>(sh_t);
      shape.num_classes = static_cast<int>(sh_c);
    }
  }
  return shape;
}

PpOcrDetection recognize_box(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const ColorRasterFrame& frame,
    const DetBox& box) {
  PpOcrDetection result;
  RecInput rec_input = preprocess_recognition(
      frame, box, options.rec_image_height, options.rec_max_width);
  if (rec_input.data.empty()) return result;

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
    return result;
  }

  if (rec_output.empty()) return result;

  const RecognitionShape shape = recognition_shape(
      rec_output,
      rec_output_shape,
      static_cast<int>(session.impl_->char_dict.size()));
  if (shape.timesteps <= 0 || shape.num_classes <= 0) return result;

  result.text = ctc_decode(
      rec_output.data(),
      shape.timesteps,
      shape.num_classes,
      session.impl_->char_dict);
  if (result.text.empty()) return result;

  result.score = recognition_confidence(
      rec_output.data(),
      shape.timesteps,
      shape.num_classes);
  result.bbox_left = std::max(0, box.left);
  result.bbox_top = std::max(0, box.top);
  result.bbox_right = std::min(frame.width, box.right);
  result.bbox_bottom = std::min(frame.height, box.bottom);
  return result;
}

std::vector<PpOcrDetection> recognize_boxes_parallel(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const ColorRasterFrame& frame,
    const std::vector<DetBox>& boxes) {
  std::vector<PpOcrDetection> results(boxes.size());
  if (boxes.empty()) return results;

  const int requested_workers = std::max(1, options.recognition_parallel_workers);
  const int worker_count = std::min<int>(
      requested_workers,
      static_cast<int>(boxes.size()));
  if (worker_count <= 1 ||
      static_cast<int>(boxes.size()) < options.recognition_parallel_min_boxes) {
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      results[i] = recognize_box(session, options, frame, boxes[i]);
    }
    return results;
  }

  std::atomic<std::size_t> next_index{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(worker_count));
  for (int worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&]() {
      while (true) {
        const std::size_t index = next_index.fetch_add(1);
        if (index >= boxes.size()) break;
        results[index] = recognize_box(session, options, frame, boxes[index]);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  return results;
}

}  // namespace

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

  bool identity_verified = false;
  if (bundles->det_manifest_loaded && bundles->rec_manifest_loaded &&
      bundles->det_manifest && bundles->rec_manifest) {
    const auto& dm = *bundles->det_manifest;
    const auto& rm = *bundles->rec_manifest;

    const bool det_ok = verify_file_hash(
        dm, bundles->det_bundle_dir, bundles->det_onnx.filename().string());
    const bool rec_onnx_ok = verify_file_hash(
        rm, bundles->rec_bundle_dir, bundles->rec_onnx.filename().string());
    const bool rec_yml_ok = !bundles->rec_yml.empty() &&
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

  svp::models::OnnxSessionOptions det_sess_opts;
  det_sess_opts.execution_provider = options.execution_provider;
  det_sess_opts.intra_op_num_threads = options.det_intra_op_num_threads;
  det_sess_opts.inter_op_num_threads = options.det_inter_op_num_threads;
  det_sess_opts.graph_optimization_level = options.det_graph_optimization_level;
  det_sess_opts.execution_mode = options.det_execution_mode;

  svp::models::OnnxSessionOptions rec_sess_opts;
  rec_sess_opts.execution_provider = options.execution_provider;
  rec_sess_opts.intra_op_num_threads = options.rec_intra_op_num_threads;
  rec_sess_opts.inter_op_num_threads = options.rec_inter_op_num_threads;
  rec_sess_opts.graph_optimization_level = options.rec_graph_optimization_level;
  rec_sess_opts.execution_mode = options.rec_execution_mode;

  try {
    session.impl_->det_session = svp::models::OnnxSession::load(
        det_manifest, bundles->det_bundle_dir, det_sess_opts);
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
        rec_manifest, bundles->rec_bundle_dir, rec_sess_opts);
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
  const auto frame_start = SteadyClock::now();
  PpOcrFrameResult result;
  result.frame_id = frame.frame_id;
  result.timestamp_us = frame.timestamp_us;
  result.frame_width = frame.width;
  result.frame_height = frame.height;

  if (!session.available || frame.pixels.empty()) return result;

  const auto preprocess_start = SteadyClock::now();
  DetInputImage det_input = preprocess_detection(frame, options.det_limit_side_len);
  result.preprocess_detection_ms = elapsed_ms(preprocess_start, SteadyClock::now());
  std::vector<std::int64_t> det_shape = {1, 3, det_input.height, det_input.width};
  std::vector<float> det_output;
  std::vector<std::int64_t> det_output_shape;
  try {
    const auto inference_start = SteadyClock::now();
    auto [data, shape] = session.impl_->det_session.run_raw_with_shape(
        session.impl_->det_input_name,
        det_input.data.data(),
        det_input.data.size(),
        det_shape);
    result.detection_inference_ms = elapsed_ms(inference_start, SteadyClock::now());
    det_output = std::move(data);
    det_output_shape = std::move(shape);
  } catch (const std::exception&) {
    return result;
  }

  if (det_output.empty()) return result;

  const int total_pixels = static_cast<int>(det_output.size());
  int pred_h = det_input.height;
  int pred_w = det_input.width;
  if (det_output_shape.size() >= 2) {
    const auto sh = det_output_shape[det_output_shape.size() - 2];
    const auto sw = det_output_shape[det_output_shape.size() - 1];
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

  const auto postprocess_start = SteadyClock::now();
  const auto boxes = db_postprocess(
      det_output.data(),
      pred_h,
      pred_w,
      det_input.ratio,
      options.det_thresh,
      options.det_box_thresh,
      options.det_unclip_ratio);
  result.detection_postprocess_ms = elapsed_ms(postprocess_start, SteadyClock::now());
  svp::core::check_memory_limit("ocr.ppocr.detector.after_postprocess", {
      {"frame_id", frame.frame_id},
      {"timestamp_us", std::to_string(frame.timestamp_us)},
      {"frame_width", std::to_string(frame.width)},
      {"frame_height", std::to_string(frame.height)},
      {"det_input_width", std::to_string(det_input.width)},
      {"det_input_height", std::to_string(det_input.height)},
      {"det_output_elements", std::to_string(det_output.size())},
      {"box_count", std::to_string(boxes.size())}
  });

  std::size_t recognized_attempts = 0;
  const auto recognition_start = SteadyClock::now();
  auto recognized = recognize_boxes_parallel(session, options, frame, boxes);
  recognized_attempts = boxes.size();
  for (auto& det : recognized) {
    if (!det.text.empty() && det.score >= options.min_text_score) {
      result.detections.push_back(std::move(det));
    }
  }
  result.recognition_total_ms = elapsed_ms(recognition_start, SteadyClock::now());
  result.recognition_attempt_count = recognized_attempts;
  result.frame_total_ms = elapsed_ms(frame_start, SteadyClock::now());
  svp::core::check_memory_limit("ocr.ppocr.frame.complete", {
      {"frame_id", frame.frame_id},
      {"timestamp_us", std::to_string(frame.timestamp_us)},
      {"box_count", std::to_string(boxes.size())},
      {"recognition_attempts", std::to_string(recognized_attempts)},
      {"detection_count", std::to_string(result.detections.size())},
      {"preprocess_detection_ms", std::to_string(result.preprocess_detection_ms)},
      {"detection_inference_ms", std::to_string(result.detection_inference_ms)},
      {"detection_postprocess_ms", std::to_string(result.detection_postprocess_ms)},
      {"recognition_total_ms", std::to_string(result.recognition_total_ms)},
      {"frame_total_ms", std::to_string(result.frame_total_ms)},
      {"recognition_parallel_workers", std::to_string(options.recognition_parallel_workers)},
      {"recognition_parallel_min_boxes", std::to_string(options.recognition_parallel_min_boxes)}
  });

  return result;
}

PpOcrDetection run_pp_ocr_recognition_on_crop(
    const PpOcrSession& session,
    const PpOcrOptions& options,
    const ColorRasterFrame& crop) {
  if (!session.available || crop.pixels.empty() ||
      crop.width <= 0 || crop.height <= 0) {
    return {};
  }

  const DetBox full_crop_box{0, 0, crop.width, crop.height};
  return recognize_box(session, options, crop, full_crop_box);
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
