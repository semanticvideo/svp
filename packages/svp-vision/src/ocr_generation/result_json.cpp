#include "ocr_generation_internal.hpp"

#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/ocr_temporal_sampling.hpp"

namespace svp::vision {

nlohmann::json ocr_generation_result_to_json(const OcrGenerationResult& result) {
  nlohmann::json regions_arr = nlohmann::json::array();
  for (const auto& reg : result.text_regions) {
    regions_arr.push_back(text_region_to_json(reg));
  }
  nlohmann::json obs_arr = nlohmann::json::array();
  for (const auto& obs : result.text_observations) {
    obs_arr.push_back(text_observation_to_json(obs));
  }
  nlohmann::json num_arr = nlohmann::json::array();
  for (const auto& nv : result.numeric_values) {
    num_arr.push_back(numeric_value_to_json(nv));
  }

  return {
      {"ocr_available", result.ocr_available},
      {"ocr_frame_input_available", result.ocr_frame_input_available},
      {"ocr_detection_run", result.ocr_detection_run},
      {"ocr_recognition_run", result.ocr_recognition_run},
      {"text_regions_written", result.text_regions_written},
      {"text_observations_written", result.text_observations_written},
      {"numeric_values_written", result.numeric_values_written},
      {"text_absence_written", result.text_absence_written},
      {"text_region_count", result.text_region_count},
      {"text_observation_count", result.text_observation_count},
      {"numeric_value_count", result.numeric_value_count},
      {"total_reconciled_observations", result.total_reconciled_observations},
      {"observation_count_capped", result.observation_count_capped},
      {"target_max_observations", result.target_max_observations},
      {"blocker", sanitize_utf8(result.blocker)},
      {"text_regions", regions_arr},
      {"text_observations", obs_arr},
      {"numeric_values", num_arr},
      {"text_absence", text_absence_to_json(result.text_absence)},
      {"processors", result.processors},
      {"evidence_crops_written", result.evidence_crops_written},
      {"evidence_crop_count", result.evidence_crop_count},
      {"evidence_crop_total_bytes", result.evidence_crop_total_bytes},
      {"evidence_crops_skipped", result.evidence_crops_skipped},
      {"evidence_crops_skipped_reason", sanitize_utf8(result.evidence_crops_skipped_reason)},
      {"crop_coverage_policy", sanitize_utf8(result.crop_coverage_policy)},
      {"crop_effective_max_total_crops", result.crop_effective_max_total_crops},
      {"crop_effective_max_total_crop_bytes", result.crop_effective_max_total_crop_bytes},
      {"crops_skipped_by_count_cap", result.crops_skipped_by_count_cap},
      {"crops_skipped_by_byte_cap", result.crops_skipped_by_byte_cap},
      {"crops_skipped_by_extraction", result.crops_skipped_by_extraction},
      {"crop_total_observations_requested", result.crop_total_observations_requested},
      {"every_observation_has_crop", result.every_observation_has_crop},
      {"crop_coverage_status", sanitize_utf8(result.crop_coverage_status)},
      {"roi_hardening_run", result.roi_hardening_run},
      {"temporal_sampling", ocr_temporal_sampling_result_to_json(result.temporal_sampling)},
      {"evidence_crops", evidence_crop_result_to_json(
          EvidenceCropResult{
              result.evidence_crops,
              result.evidence_crop_total_bytes,
              result.evidence_crops_written,
              result.evidence_crop_count,
              result.evidence_crops_skipped,
              result.evidence_crops_skipped_reason,
              result.crop_coverage_policy,
              result.crop_effective_max_total_crops,
              result.crop_effective_max_total_crop_bytes,
              result.crops_skipped_by_count_cap,
              result.crops_skipped_by_byte_cap,
              result.crops_skipped_by_extraction,
              result.crop_total_observations_requested,
              result.every_observation_has_crop,
              result.crop_coverage_status,
              {}})},
  };
}

}  // namespace svp::vision
