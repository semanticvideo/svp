#include "ocr_generation_internal.hpp"

#include "svp/core/memory_diagnostics.hpp"
#include "svp/vision/ocr_temporal_sampling.hpp"

#include <string>
#include <utility>

namespace svp::vision::ocr_generation_internal {
namespace {

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

}  // namespace

OcrGenerationResult finish_ocr_not_executed(
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

std::string ocr_frame_input_blocker(const DecodedCanonicalFrames& frames) {
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

OcrGenerationResult complete_ocr_generation(
    OcrGenerationResult result,
    const OcrGenerationOptions& options,
    const CollectedOcrFrames& collected,
    const DecodedCanonicalFrames& decode_status,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir) {
  svp::core::check_memory_limit("ocr.generation.after_frames", {
      {"processed_frame_count", std::to_string(collected.processed_frame_count)},
      {"all_detections", std::to_string(collected.detections.size())},
      {"frame_diagnostics", std::to_string(collected.frame_diagnostics.size())}
  });

  if (!result.ocr_frame_input_available) {
    result.blocker = ocr_frame_input_blocker(decode_status);
    return finish_ocr_not_executed(
        std::move(result),
        staging_dir,
        result.blocker,
        result.blocker);
  }

  if (collected.any_frame_failed) {
    result.blocker = "OCR processing failed: " + collected.failure_reason_details;
    set_processor_failed_absence(result);
    result.text_absence.text_region_count = 0;
    result.text_absence.text_observation_count = 0;
    result.text_absence.numeric_value_count = 0;

    nlohmann::json detector_proc = make_ocr_processor_provenance(
        "processor_ocr_detector_0001", "ocr_detector",
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "failed",
        "Text detection failed: " + collected.failure_reason_details);
    detector_proc["diagnostics"] = collected.frame_diagnostics;
    result.processors.push_back(detector_proc);

    nlohmann::json recognizer_proc = make_ocr_processor_provenance(
        "processor_ocr_recognizer_0001", "ocr_recognizer",
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "failed",
        "Text recognition failed: " + collected.failure_reason_details);
    recognizer_proc["diagnostics"] = collected.frame_diagnostics;
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

  auto reconciled = reconcile_detections(collected.detections, collected.processed_frame_count);
  svp::core::check_memory_limit("ocr.generation.after_reconcile", {
      {"all_detections", std::to_string(collected.detections.size())},
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
          std::to_string(collected.processed_frame_count) +
          " decoded frame(s) at " +
          std::to_string(collected.processed_frame_width) +
          "x" +
          std::to_string(collected.processed_frame_height) +
          " resolution; collected " +
          std::to_string(collected.detections.size()) + " per-frame detection(s)");
  detector_proc["model_refs"] = detector_model_refs;
  detector_proc["diagnostics"] = collected.frame_diagnostics;
  result.processors.push_back(detector_proc);

  nlohmann::json recognizer_proc = make_ocr_processor_provenance(
      "processor_ocr_recognizer_0001", "ocr_recognizer",
      "svp-vision-pp-ocr-v1", "onnxruntime",
      "completed",
      "Text recognition via PP-OCR ONNX (CTC decode) with multi-frame reconciliation; "
      "produced " +
          std::to_string(result.text_observation_count) +
          " reconciled text observation(s) from " +
          std::to_string(collected.detections.size()) + " per-frame detection(s); "
      "ROI crop re-read verified " + std::to_string(roi_summary.verified_crop_count) +
          " crop(s) and improved " +
          std::to_string(roi_summary.improved_observation_count) +
          " observation(s)");
  recognizer_proc["model_refs"] = detector_model_refs;
  recognizer_proc["diagnostics"] = collected.frame_diagnostics;
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

}  // namespace svp::vision::ocr_generation_internal
