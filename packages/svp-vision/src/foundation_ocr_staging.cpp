#include "svp/vision/foundation_ocr_staging.hpp"

#include <utility>

namespace svp::vision {

nlohmann::json text_region_to_json(const TextRegionRecord& record) {
  nlohmann::json j = {
      {"text_region_id", record.text_region_id},
      {"observation_type", record.observation_type},
      {"start_us", record.start_us},
      {"end_us", record.end_us},
      {"bbox_norm", record.bbox_norm},
      {"bbox_px", record.bbox_px},
      {"confidence", record.confidence},
      {"provenance_id", record.provenance_id},
  };
  if (record.frame_start.has_value()) {
    j["frame_start"] = *record.frame_start;
  }
  if (record.frame_end.has_value()) {
    j["frame_end"] = *record.frame_end;
  }
  if (record.shot_id.has_value()) {
    j["shot_id"] = *record.shot_id;
  }
  if (record.scene_id.has_value()) {
    j["scene_id"] = *record.scene_id;
  }
  if (record.region_id.has_value()) {
    j["region_id"] = *record.region_id;
  }
  if (record.entity_id.has_value()) {
    j["entity_id"] = *record.entity_id;
  }
  if (record.orientation_deg.has_value()) {
    j["orientation_deg"] = *record.orientation_deg;
  }
  if (record.text_direction.has_value()) {
    j["text_direction"] = *record.text_direction;
  }
  if (record.script.has_value()) {
    j["script"] = *record.script;
  }
  if (record.foreground_color_observation_id.has_value()) {
    j["foreground_color_observation_id"] = *record.foreground_color_observation_id;
  }
  if (record.background_color_observation_id.has_value()) {
    j["background_color_observation_id"] = *record.background_color_observation_id;
  }
  if (record.contrast_ratio.has_value()) {
    j["contrast_ratio"] = *record.contrast_ratio;
  }
  if (record.legibility_score.has_value()) {
    j["legibility_score"] = *record.legibility_score;
  }
  return j;
}

nlohmann::json text_observation_to_json(const TextObservationRecord& record) {
  nlohmann::json j = {
      {"text_observation_id", record.text_observation_id},
      {"text_region_id", record.text_region_id},
      {"observation_type", record.observation_type},
      {"raw_text", record.raw_text},
      {"normalized_text", record.normalized_text},
      {"confidence", record.confidence},
      {"source_frame_ids", record.source_frame_ids},
      {"provenance_id", record.provenance_id},
  };
  if (record.language.has_value()) {
    j["language"] = *record.language;
  }
  if (record.layout_class.has_value()) {
    j["layout_class"] = *record.layout_class;
  }
  return j;
}

nlohmann::json numeric_value_to_json(const NumericValueRecord& record) {
  nlohmann::json j = {
      {"numeric_value_id", record.numeric_value_id},
      {"text_observation_id", record.text_observation_id},
      {"text_region_id", record.text_region_id},
      {"raw_text", record.raw_text},
      {"normalized_text", record.normalized_text},
      {"number_kind", record.number_kind},
      {"numeric_value", record.numeric_value},
      {"confidence", record.confidence},
      {"parse_rule", record.parse_rule},
      {"provenance_id", record.provenance_id},
  };
  if (record.unit.has_value()) {
    j["unit"] = *record.unit;
  }
  return j;
}

nlohmann::json text_absence_to_json(const TextAbsenceRecord& record) {
  return {
      {"schema_version", record.schema_version},
      {"ocr_required", record.ocr_required},
      {"ocr_completed", record.ocr_completed},
      {"text_region_count", record.text_region_count},
      {"text_observation_count", record.text_observation_count},
      {"numeric_value_count", record.numeric_value_count},
      {"reason", record.reason},
      {"provenance_id", record.provenance_id},
  };
}

namespace {

nlohmann::json make_ocr_processor_provenance(const std::string& id,
                                             const std::string& type,
                                             const std::string& version,
                                             const std::string& runtime) {
  return {
      {"id", id},
      {"processor_type", type},
      {"processor_version", version},
      {"runtime", runtime},
      {"execution_provider", "cpu"},
      {"model_refs", nlohmann::json::array()},
  };
}

}  // namespace

FoundationOcrStagingArtifact build_foundation_ocr_staging_synthetic_artifact() {
  FoundationOcrStagingArtifact artifact;

  TextRegionRecord region;
  region.text_region_id = "text_region_000001";
  region.observation_type = "text_detection";
  region.start_us = 0;
  region.end_us = 1000000;
  region.frame_start = 0;
  region.frame_end = 30;
  region.shot_id = "shot_000001";
  region.scene_id = "scene_000001";
  region.bbox_norm = {0.1, 0.2, 0.4, 0.3};
  region.bbox_px = {128, 72, 512, 108};
  region.confidence = 0.95;
  region.provenance_id = "processor_ocr_detector_0001";
  artifact.text_regions.push_back(region);

  TextObservationRecord obs;
  obs.text_observation_id = "text_obs_000001";
  obs.text_region_id = "text_region_000001";
  obs.observation_type = "text_recognition";
  obs.raw_text = "SALE $9.99";
  obs.normalized_text = "sale 9.99";
  obs.language = nlohmann::json{
      {"primary", "en"}, {"script", "Latn"}, {"mode", "single"}, {"confidence", 0.99}};
  obs.confidence = 0.92;
  obs.layout_class = "ui_text";
  obs.source_frame_ids = {"frame_000001"};
  obs.provenance_id = "processor_ocr_recognizer_0001";
  artifact.text_observations.push_back(obs);

  NumericValueRecord num;
  num.numeric_value_id = "numeric_value_000001";
  num.text_observation_id = "text_obs_000001";
  num.text_region_id = "text_region_000001";
  num.raw_text = "$9.99";
  num.normalized_text = "9.99";
  num.number_kind = "decimal";
  num.numeric_value = "9.99";
  num.unit = "currency_unknown";
  num.confidence = 0.98;
  num.parse_rule = "svp-number-parser-v1";
  num.provenance_id = "processor_numeric_parser_0001";
  artifact.numeric_values.push_back(num);

  artifact.text_absence.schema_version = "svp-text-absence-v1";
  artifact.text_absence.ocr_required = true;
  artifact.text_absence.ocr_completed = true;
  artifact.text_absence.text_region_count = 1;
  artifact.text_absence.text_observation_count = 1;
  artifact.text_absence.numeric_value_count = 1;
  artifact.text_absence.reason = "ocr_completed";
  artifact.text_absence.provenance_id = "processor_ocr_detector_0001";

  artifact.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_detector_0001", "ocr_detector",
      "svp-vision-foundation-ocr-staging-v1", "deterministic_mock_cpp"));
  artifact.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_recognizer_0001", "ocr_recognizer",
      "svp-vision-foundation-ocr-staging-v1", "deterministic_mock_cpp"));
  artifact.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_layout_classifier_0001", "ocr_layout_classifier",
      "svp-vision-foundation-ocr-staging-v1", "deterministic_mock_cpp"));
  artifact.processors.push_back(make_ocr_processor_provenance(
      "processor_numeric_parser_0001", "numeric_parser",
      "svp-vision-foundation-ocr-staging-v1", "deterministic_mock_cpp"));

  artifact.manifest = {
      {"schema_version", "svp-builder-foundation-ocr-staging-v1"},
      {"execution_state", "foundation_synthetic_sample_only"},
      {"input_kind", "synthetic_in_memory_color_frames"},
      {"real_media_frame_decoding_run", false},
      {"model_runtime_available", false},
      {"ocr_detection_run", true},
      {"ocr_recognition_run", true},
      {"text_regions_written", true},
      {"text_observations_written", true},
      {"numeric_values_written", true},
      {"package_writer_run", false},
      {"valid_svp_package_written", false},
      {"text_region_count", 1},
      {"text_observation_count", 1},
      {"numeric_value_count", 1},
      {"text_absence_written", true},
      {"processor_provenance_written", true},
      {"notes",
       {"This is a builder staging artifact for the foundation OCR pipeline.",
        "It proves RC2-shaped OCR records can be produced from deterministic sample inputs.",
        "It does not claim observations were decoded from the requested media source.",
        "It does not claim a final .svp package was written."}},
  };

  return artifact;
}

nlohmann::json foundation_ocr_staging_artifact_to_json(
    const FoundationOcrStagingArtifact& artifact) {
  nlohmann::json regions_arr = nlohmann::json::array();
  for (const auto& reg : artifact.text_regions) {
    regions_arr.push_back(text_region_to_json(reg));
  }
  nlohmann::json obs_arr = nlohmann::json::array();
  for (const auto& obs : artifact.text_observations) {
    obs_arr.push_back(text_observation_to_json(obs));
  }
  nlohmann::json num_arr = nlohmann::json::array();
  for (const auto& num : artifact.numeric_values) {
    num_arr.push_back(numeric_value_to_json(num));
  }

  return {
      {"manifest", artifact.manifest},
      {"text",
       {{"text_regions", regions_arr},
        {"text_observations", obs_arr},
        {"numeric_values", num_arr},
        {"text_absence", text_absence_to_json(artifact.text_absence)}}},
      {"provenance", {{"processors", artifact.processors}}},
  };
}

FoundationOcrStagingArtifact build_real_ocr_staging_artifact(
    const media::MediaIngestPlan& plan,
    const bool model_runtime_available) {
  FoundationOcrStagingArtifact artifact;

  artifact.text_absence.schema_version = "svp-text-absence-v1";
  artifact.text_absence.ocr_required = true;
  artifact.text_absence.ocr_completed = false;
  artifact.text_absence.text_region_count = 0;
  artifact.text_absence.text_observation_count = 0;
  artifact.text_absence.numeric_value_count = 0;
  artifact.text_absence.reason = "processor_failed";
  artifact.text_absence.provenance_id = "processor_ocr_detector_0001";

  const std::string runtime_label = model_runtime_available ? "onnxruntime" : "not_executed";

  artifact.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_detector_0001", "ocr_detector",
      "svp-vision-real-ocr-staging-v1", runtime_label));
  artifact.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_recognizer_0001", "ocr_recognizer",
      "svp-vision-real-ocr-staging-v1", runtime_label));
  artifact.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_layout_classifier_0001", "ocr_layout_classifier",
      "svp-vision-real-ocr-staging-v1", runtime_label));
  artifact.processors.push_back(make_ocr_processor_provenance(
      "processor_numeric_parser_0001", "numeric_parser",
      "svp-vision-real-ocr-staging-v1", runtime_label));

  artifact.manifest = {
      {"schema_version", "svp-builder-foundation-ocr-staging-v1"},
      {"execution_state", "real_ocr_model_runtime_unavailable"},
      {"input_kind", "real_decoded_canonical_raster_frames"},
      {"real_media_frame_decoding_run", false},
      {"model_runtime_available", model_runtime_available},
      {"ocr_detection_run", false},
      {"ocr_recognition_run", false},
      {"text_regions_written", true},
      {"text_observations_written", true},
      {"numeric_values_written", true},
      {"package_writer_run", false},
      {"valid_svp_package_written", false},
      {"text_region_count", 0},
      {"text_observation_count", 0},
      {"numeric_value_count", 0},
      {"text_absence_written", true},
      {"processor_provenance_written", true},
      {"source_path", plan.source_path.string()},
      {"notes",
       {"Real media frames were not processed because the ONNX model runtime is unavailable.",
        "No fake OCR observations were written.",
        "It does not claim a final .svp package was written."}},
  };

  return artifact;
}

}  // namespace svp::vision
