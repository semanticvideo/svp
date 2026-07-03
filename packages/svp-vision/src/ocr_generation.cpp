#include "svp/vision/ocr_generation.hpp"

#include "ocr_generation/ocr_generation_internal.hpp"

#include "svp/core/memory_diagnostics.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/ocr_temporal_sampling.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {
namespace {

using ocr_generation_internal::FrameDetection;
using ocr_generation_internal::RoiHardeningSummary;
using ocr_generation_internal::emit_reconciled_records;
using ocr_generation_internal::generate_and_harden_evidence_crops;
using ocr_generation_internal::make_ocr_processor_provenance;
using ocr_generation_internal::reconcile_detections;
using ocr_generation_internal::refresh_numeric_values_from_observations;
using ocr_generation_internal::write_failure_stage_files;
using ocr_generation_internal::write_success_stage_files;

void mark_failure_stage_written(OcrGenerationResult& result) {
  result.text_regions_written = true;
  result.text_observations_written = true;
  result.numeric_values_written = true;
  result.text_absence_written = true;
}

void attach_temporal_sampling_provenance(OcrGenerationResult& result) {
  if (!result.processors.empty()) {
    result.processors[0]["temporal_sampling"] =
        ocr_temporal_sampling_result_to_json(result.temporal_sampling);
  }
}

void push_not_run_processors(OcrGenerationResult& result,
                             const std::string& detector_note,
                             const std::string& recognizer_note) {
  result.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_detector_0001", "ocr_detector",
      "svp-vision-pp-ocr-v1", "onnxruntime",
      "not_executed", detector_note));
  result.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_recognizer_0001", "ocr_recognizer",
      "svp-vision-pp-ocr-v1", "onnxruntime",
      "not_executed", recognizer_note));
  result.processors.push_back(make_ocr_processor_provenance(
      "processor_numeric_parser_0001", "numeric_parser",
      "svp-vision-ocr-generation-v1", "deterministic_cpp",
      "not_run", "No OCR text to parse"));
}

void set_processor_failed_absence(OcrGenerationResult& result) {
  result.text_absence.schema_version = "svp-text-absence-v1";
  result.text_absence.ocr_required = true;
  result.text_absence.ocr_completed = false;
  result.text_absence.reason = "processor_failed";
  result.text_absence.provenance_id = "processor_ocr_detector_0001";
}

OcrGenerationResult finish_not_executed(
    OcrGenerationResult result,
    const std::filesystem::path& staging_dir,
    const std::string& detector_note,
    const std::string& recognizer_note) {
  set_processor_failed_absence(result);
  push_not_run_processors(result, detector_note, recognizer_note);
  attach_temporal_sampling_provenance(result);
  write_failure_stage_files(staging_dir, result.text_absence);
  mark_failure_stage_written(result);
  return result;
}

std::string frame_input_blocker(const DecodedCanonicalFrames& frames) {
  if (!frames.decoding_attempted) {
    return "Frame decoding was not attempted; OCR is blocked: " +
        frames.skipped_reason;
  }
  if (!frames.decoding_succeeded) {
    return "Frame decoding failed; OCR is blocked: " +
        frames.skipped_reason;
  }
  return "No decoded frames available for OCR";
}

int positive_env_int_or_default(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || parsed <= 0) return fallback;
  return static_cast<int>(parsed);
}

int graph_opt_env_or_default(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  if (std::string(value) == "disable") return 0;
  if (std::string(value) == "basic") return 1;
  if (std::string(value) == "extended") return 2;
  if (std::string(value) == "layout") return 3;
  if (std::string(value) == "all") return 99;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) return fallback;
  return static_cast<int>(parsed);
}

std::string execution_mode_env_or_default(const char* name,
                                          const std::string& fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  const std::string parsed(value);
  if (parsed == "parallel" || parsed == "sequential") return parsed;
  return fallback;
}

std::string execution_provider_env_or_default(const char* name,
                                              const std::string& fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  const std::string parsed(value);
  if (parsed == "cpu" || parsed == "coreml") return parsed;
  return fallback;
}

std::optional<std::vector<std::int64_t>> env_timestamp_list_us(
    const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return std::nullopt;

  std::vector<std::int64_t> timestamps;
  const char* cursor = value;
  while (*cursor != '\0') {
    char* end = nullptr;
    const long long parsed = std::strtoll(cursor, &end, 10);
    if (end == cursor || parsed < 0) return std::nullopt;
    timestamps.push_back(static_cast<std::int64_t>(parsed));
    cursor = end;
    if (*cursor == ',') {
      ++cursor;
    } else if (*cursor != '\0') {
      return std::nullopt;
    }
  }
  if (timestamps.empty()) return std::nullopt;
  std::sort(timestamps.begin(), timestamps.end());
  timestamps.erase(std::unique(timestamps.begin(), timestamps.end()),
                   timestamps.end());
  return timestamps;
}

}  // namespace

OcrSourceFrameDimensions derive_ocr_source_frame_dimensions(
    int stored_width,
    int stored_height,
    int rotation_degrees) {
  const int normalized_rotation = ((rotation_degrees % 360) + 360) % 360;
  const bool swaps_axes =
      normalized_rotation == 90 || normalized_rotation == 270;
  return swaps_axes
      ? OcrSourceFrameDimensions{stored_height, stored_width}
      : OcrSourceFrameDimensions{stored_width, stored_height};
}

OcrGenerationResult generate_ocr_observations(
    const OcrGenerationOptions& options,
    const DecodedCanonicalFrames& frame_input,
    const std::filesystem::path& staging_dir) {
  OcrGenerationResult result;

  PpOcrOptions pp_ocr_opts;
  pp_ocr_opts.model_cache_root = options.model_cache_root;
  pp_ocr_opts.execution_provider = execution_provider_env_or_default(
      "SVP_OCR_EXECUTION_PROVIDER", pp_ocr_opts.execution_provider);
  pp_ocr_opts.intra_op_num_threads = positive_env_int_or_default(
      "SVP_OCR_ONNX_INTRA_OP_THREADS", pp_ocr_opts.intra_op_num_threads);
  pp_ocr_opts.inter_op_num_threads = positive_env_int_or_default(
      "SVP_OCR_ONNX_INTER_OP_THREADS", pp_ocr_opts.inter_op_num_threads);
  pp_ocr_opts.graph_optimization_level = graph_opt_env_or_default(
      "SVP_OCR_ONNX_GRAPH_OPT_LEVEL", pp_ocr_opts.graph_optimization_level);
  pp_ocr_opts.execution_mode = execution_mode_env_or_default(
      "SVP_OCR_ONNX_EXECUTION_MODE", pp_ocr_opts.execution_mode);
  pp_ocr_opts.recognition_parallel_workers = positive_env_int_or_default(
      "SVP_OCR_RECOGNITION_PARALLEL_WORKERS",
      pp_ocr_opts.recognition_parallel_workers);
  pp_ocr_opts.recognition_parallel_min_boxes = positive_env_int_or_default(
      "SVP_OCR_RECOGNITION_PARALLEL_MIN_BOXES",
      pp_ocr_opts.recognition_parallel_min_boxes);

  PpOcrSession pp_ocr_session = create_pp_ocr_session(pp_ocr_opts);
  svp::core::check_memory_limit("ocr.generation.session_created", {
      {"available", pp_ocr_session.available ? "true" : "false"},
      {"blocker", pp_ocr_session.blocker},
      {"execution_provider", pp_ocr_opts.execution_provider},
      {"intra_op_num_threads", std::to_string(pp_ocr_opts.intra_op_num_threads)},
      {"inter_op_num_threads", std::to_string(pp_ocr_opts.inter_op_num_threads)},
      {"graph_optimization_level", std::to_string(pp_ocr_opts.graph_optimization_level)},
      {"execution_mode", pp_ocr_opts.execution_mode},
      {"recognition_parallel_workers", std::to_string(pp_ocr_opts.recognition_parallel_workers)},
      {"recognition_parallel_min_boxes", std::to_string(pp_ocr_opts.recognition_parallel_min_boxes)}
  });

  result.ocr_available = pp_ocr_session.available;
  result.ocr_frame_input_available =
      frame_input.decoding_succeeded && !frame_input.frames.empty();

  if (!result.ocr_available) {
    result.blocker = pp_ocr_session.blocker;
    return finish_not_executed(
        std::move(result),
        staging_dir,
        result.blocker,
        "PP-OCR not available");
  }

  bool use_streamed_high_res_frames = false;
  DecodedCanonicalFrames streamed_frame_status;
  if (options.media_plan != nullptr &&
      options.ocr_frame_width > 0 && options.ocr_frame_height > 0) {
    const std::int64_t duration_us =
        compute_media_duration_us(*options.media_plan);
    result.temporal_sampling =
        compute_ocr_temporal_timestamps(duration_us, options.sampling_config);
    const auto diagnostic_timestamps =
        env_timestamp_list_us("SVP_OCR_DIAG_TIMESTAMPS_US");
    if (diagnostic_timestamps.has_value()) {
      result.temporal_sampling.timestamps_us = *diagnostic_timestamps;
      result.temporal_sampling.sample_count =
          static_cast<int>(result.temporal_sampling.timestamps_us.size());
      result.temporal_sampling.temporal_coverage_note =
          "Diagnostic OCR timestamp override via SVP_OCR_DIAG_TIMESTAMPS_US; "
          "not for production coverage claims.";
    }
    if (!result.temporal_sampling.timestamps_us.empty()) {
      use_streamed_high_res_frames = true;
    } else {
      streamed_frame_status.decoding_attempted = false;
      streamed_frame_status.skipped_reason =
          "no OCR temporal timestamps computed (duration unknown?)";
    }
  }

  if (!use_streamed_high_res_frames && !result.ocr_frame_input_available) {
    result.blocker = frame_input_blocker(frame_input);
    return finish_not_executed(
        std::move(result),
        staging_dir,
        result.blocker,
        result.blocker);
  }

  result.ocr_detection_run = true;

  std::vector<FrameDetection> all_detections;
  std::vector<nlohmann::json> frame_diagnostics;
  bool any_frame_failed = false;
  std::string failure_reason_details;
  int processed_frame_count = 0;
  int processed_frame_width = 0;
  int processed_frame_height = 0;

  auto process_ocr_frame = [&](const ColorRasterFrame& frame,
                               std::size_t /*frame_idx*/) {
    ++processed_frame_count;
    if (options.on_progress) {
      const int total = use_streamed_high_res_frames
          ? static_cast<int>(result.temporal_sampling.timestamps_us.size())
          : static_cast<int>(frame_input.frames.size());
      options.on_progress(processed_frame_count, total);
    }
    if (processed_frame_width == 0 && processed_frame_height == 0) {
      processed_frame_width = frame.width;
      processed_frame_height = frame.height;
    }
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
      frame_diagnostics.push_back({
          {"frame_id", sanitize_utf8(frame.frame_id)},
          {"timestamp_us", frame.timestamp_us},
          {"extraction_succeeded", false},
          {"extraction_error", "Empty or invalid frame pixels"},
          {"pp_ocr_attempted", false}
      });
      any_frame_failed = true;
      if (failure_reason_details.empty()) {
        failure_reason_details = sanitize_utf8("Invalid frame data for " + frame.frame_id);
      }
      return;
    }

    PpOcrFrameResult ocr_result;
    try {
      ocr_result = run_pp_ocr_on_frame(pp_ocr_session, pp_ocr_opts, frame);
    } catch (const std::exception& e) {
      frame_diagnostics.push_back({
          {"frame_id", sanitize_utf8(frame.frame_id)},
          {"timestamp_us", frame.timestamp_us},
          {"extraction_succeeded", true},
          {"pp_ocr_attempted", true},
          {"pp_ocr_succeeded", false},
          {"pp_ocr_error", sanitize_utf8(e.what())}
      });
      any_frame_failed = true;
      if (failure_reason_details.empty()) {
        failure_reason_details =
            sanitize_utf8("PP-OCR failed on frame " + frame.frame_id + ": " + e.what());
      }
      return;
    }

    frame_diagnostics.push_back({
        {"frame_id", sanitize_utf8(frame.frame_id)},
        {"timestamp_us", frame.timestamp_us},
        {"extraction_succeeded", true},
        {"pp_ocr_attempted", true},
        {"pp_ocr_succeeded", true},
        {"detection_count", ocr_result.detections.size()}
    });

    for (const auto& det : ocr_result.detections) {
      if (det.bbox_right <= det.bbox_left ||
          det.bbox_bottom <= det.bbox_top) {
        continue;
      }

      FrameDetection fdet;
      fdet.frame_id = frame.frame_id;
      fdet.timestamp_us = frame.timestamp_us;
      fdet.frame_index = frame.frame_index;
      fdet.frame_width = frame.width;
      fdet.frame_height = frame.height;
      fdet.raw_text = det.text;
      fdet.confidence = det.score;
      fdet.bbox_left = det.bbox_left;
      fdet.bbox_top = det.bbox_top;
      fdet.bbox_right = det.bbox_right;
      fdet.bbox_bottom = det.bbox_bottom;
      all_detections.push_back(std::move(fdet));
    }
  };

  if (use_streamed_high_res_frames) {
    svp::core::check_memory_limit("ocr.generation.streaming_begin", {
        {"sample_count", std::to_string(result.temporal_sampling.timestamps_us.size())},
        {"diagnostic_timestamp_override",
         std::getenv("SVP_OCR_DIAG_TIMESTAMPS_US") != nullptr ? "true" : "false"},
        {"ocr_frame_width", std::to_string(options.ocr_frame_width)},
        {"ocr_frame_height", std::to_string(options.ocr_frame_height)}
    });
    streamed_frame_status = decode_frames_at_timestamps_streaming(
        *options.media_plan,
        options.ffmpeg_path,
        options.ocr_frame_width,
        options.ocr_frame_height,
        result.temporal_sampling.timestamps_us,
        process_ocr_frame,
        options.frame_catalog,
        "ocr");
    result.ocr_frame_input_available =
        streamed_frame_status.decoding_succeeded &&
        streamed_frame_status.frames_decoded > 0;
  } else {
    for (std::size_t frame_idx = 0; frame_idx < frame_input.frames.size(); ++frame_idx) {
      process_ocr_frame(frame_input.frames[frame_idx], frame_idx);
    }
    result.ocr_frame_input_available =
        frame_input.decoding_succeeded && !frame_input.frames.empty();
    processed_frame_count = static_cast<int>(frame_input.frames.size());
    if (!frame_input.frames.empty()) {
      processed_frame_width = frame_input.frames[0].width;
      processed_frame_height = frame_input.frames[0].height;
    }
  }

  svp::core::check_memory_limit("ocr.generation.after_frames", {
      {"processed_frame_count", std::to_string(processed_frame_count)},
      {"all_detections", std::to_string(all_detections.size())},
      {"frame_diagnostics", std::to_string(frame_diagnostics.size())}
  });

  if (!result.ocr_frame_input_available) {
    const DecodedCanonicalFrames& failed_frames =
        use_streamed_high_res_frames ? streamed_frame_status : frame_input;
    result.blocker = frame_input_blocker(failed_frames);
    return finish_not_executed(
        std::move(result),
        staging_dir,
        result.blocker,
        result.blocker);
  }

  if (any_frame_failed) {
    result.blocker = "OCR processing failed: " + failure_reason_details;
    set_processor_failed_absence(result);
    result.text_absence.text_region_count = 0;
    result.text_absence.text_observation_count = 0;
    result.text_absence.numeric_value_count = 0;

    nlohmann::json detector_proc = make_ocr_processor_provenance(
        "processor_ocr_detector_0001", "ocr_detector",
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "failed",
        "Text detection failed: " + failure_reason_details);
    detector_proc["diagnostics"] = frame_diagnostics;
    result.processors.push_back(detector_proc);

    nlohmann::json recognizer_proc = make_ocr_processor_provenance(
        "processor_ocr_recognizer_0001", "ocr_recognizer",
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "failed",
        "Text recognition failed: " + failure_reason_details);
    recognizer_proc["diagnostics"] = frame_diagnostics;
    result.processors.push_back(recognizer_proc);

    result.processors.push_back(make_ocr_processor_provenance(
        "processor_numeric_parser_0001", "numeric_parser",
        "svp-vision-ocr-generation-v1", "deterministic_cpp",
        "not_run", "No OCR text to parse due to OCR execution failure"));

    attach_temporal_sampling_provenance(result);
    write_failure_stage_files(staging_dir, result.text_absence);
    mark_failure_stage_written(result);
    result.evidence_crops_written = true;
    return result;
  }

  auto reconciled = reconcile_detections(all_detections, processed_frame_count);
  svp::core::check_memory_limit("ocr.generation.after_reconcile", {
      {"all_detections", std::to_string(all_detections.size())},
      {"reconciled", std::to_string(reconciled.size())}
  });
  result.total_reconciled_observations =
      static_cast<std::int64_t>(reconciled.size());
  result.target_max_observations = options.target_max_observations;

  emit_reconciled_records(options, reconciled, result);

  const RoiHardeningSummary roi_summary =
      generate_and_harden_evidence_crops(
          options,
          reconciled,
          pp_ocr_session,
          pp_ocr_opts,
          staging_dir,
          result);

  refresh_numeric_values_from_observations(result);

  result.text_absence.schema_version = "svp-text-absence-v1";
  result.text_absence.ocr_required = true;
  result.text_absence.ocr_completed = true;
  result.text_absence.text_region_count = result.text_region_count;
  result.text_absence.text_observation_count = result.text_observation_count;
  result.text_absence.numeric_value_count = result.numeric_value_count;
  result.text_absence.reason =
      result.text_observations.empty() ? "no_text_detected" : "ocr_completed";
  result.text_absence.provenance_id = "processor_ocr_detector_0001";

  nlohmann::json detector_model_refs = nlohmann::json::array();
  detector_model_refs.push_back(pp_ocr_model_info_to_json(pp_ocr_session.model_info));

  nlohmann::json detector_proc = make_ocr_processor_provenance(
      "processor_ocr_detector_0001", "ocr_detector",
      "svp-vision-pp-ocr-v1", "onnxruntime",
      "completed",
      "Text detection via PP-OCR ONNX (DB post-processing) on " +
          std::to_string(processed_frame_count) +
          " decoded frame(s) at " +
          std::to_string(processed_frame_width) +
          "x" +
          std::to_string(processed_frame_height) +
          " resolution; collected " +
          std::to_string(all_detections.size()) + " per-frame detection(s)");
  detector_proc["model_refs"] = detector_model_refs;
  detector_proc["diagnostics"] = frame_diagnostics;
  result.processors.push_back(detector_proc);

  nlohmann::json recognizer_proc = make_ocr_processor_provenance(
      "processor_ocr_recognizer_0001", "ocr_recognizer",
      "svp-vision-pp-ocr-v1", "onnxruntime",
      "completed",
      "Text recognition via PP-OCR ONNX (CTC decode) with multi-frame reconciliation; "
      "produced " +
          std::to_string(result.text_observation_count) +
          " reconciled text observation(s) from " +
          std::to_string(all_detections.size()) + " per-frame detection(s); "
      "ROI crop re-read verified " + std::to_string(roi_summary.verified_crop_count) +
          " crop(s) and improved " +
          std::to_string(roi_summary.improved_observation_count) +
          " observation(s)");
  recognizer_proc["model_refs"] = detector_model_refs;
  recognizer_proc["diagnostics"] = frame_diagnostics;
  recognizer_proc["limitations"] = nlohmann::json::array({
      "Recognizer confidence is uncalibrated: PP-OCRv6 outputs raw logits over 18710 classes; "
      "softmax probabilities are near-zero and should not be used as a quality signal.",
      "Space characters are not emitted by the ONNX CTC decoder; multi-word text runs together."
  });
  recognizer_proc["roi_hardening"] = {
      {"run", result.roi_hardening_run},
      {"verified_crop_count", roi_summary.verified_crop_count},
      {"improved_observation_count", roi_summary.improved_observation_count},
  };
  result.processors.push_back(recognizer_proc);

  result.processors.push_back(make_ocr_processor_provenance(
      "processor_numeric_parser_0001", "numeric_parser",
      "svp-vision-ocr-generation-v1", "deterministic_cpp",
      "completed",
      "Parsed " + std::to_string(result.numeric_value_count) +
          " numeric value(s) from recognized text"));

  if (!result.processors.empty()) {
    result.processors[0]["temporal_sampling"] =
        ocr_temporal_sampling_result_to_json(result.temporal_sampling);
    result.processors[0]["ocr_coverage"] = {
        {"total_reconciled_observations", result.total_reconciled_observations},
        {"observation_count_capped", result.observation_count_capped},
        {"target_max_observations", result.target_max_observations},
    };
  }

  if (result.processors.size() >= 2) {
    result.processors[1]["evidence_crop_coverage"] = {
        {"crop_coverage_policy", result.crop_coverage_policy},
        {"effective_max_total_crops", result.crop_effective_max_total_crops},
        {"effective_max_total_crop_bytes", result.crop_effective_max_total_crop_bytes},
        {"crop_count", result.evidence_crop_count},
        {"crops_skipped_count", result.evidence_crops_skipped},
        {"crops_skipped_by_count_cap", result.crops_skipped_by_count_cap},
        {"crops_skipped_by_byte_cap", result.crops_skipped_by_byte_cap},
        {"crops_skipped_by_extraction", result.crops_skipped_by_extraction},
        {"total_observations_requested", result.crop_total_observations_requested},
        {"every_observation_has_crop", result.every_observation_has_crop},
        {"crop_coverage_status", result.crop_coverage_status},
    };
  }

  write_success_stage_files(staging_dir, result);
  return result;
}

DecodedCanonicalFrames decode_ocr_frames_temporal(
    const OcrGenerationOptions& options,
    OcrTemporalSamplingResult& out_sampling) {
  if (options.media_plan == nullptr ||
      options.ocr_frame_width <= 0 || options.ocr_frame_height <= 0) {
    return {};
  }

  const std::int64_t duration_us =
      compute_media_duration_us(*options.media_plan);

  out_sampling = compute_ocr_temporal_timestamps(duration_us, options.sampling_config);

  if (out_sampling.timestamps_us.empty()) {
    DecodedCanonicalFrames empty;
    empty.decoding_attempted = false;
    empty.skipped_reason = "no OCR temporal timestamps computed (duration unknown?)";
    return empty;
  }

  return decode_frames_at_timestamps(
      *options.media_plan,
      options.ffmpeg_path,
      options.ocr_frame_width,
      options.ocr_frame_height,
      out_sampling.timestamps_us,
      options.frame_catalog,
      "ocr");
}

}  // namespace svp::vision
