#pragma once

#include "svp/media/media_ingest_plan.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {

struct TextRegionRecord {
  std::string text_region_id;
  std::string observation_type = "text_detection";
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::optional<std::int64_t> frame_start;
  std::optional<std::int64_t> frame_end;
  std::optional<std::string> shot_id;
  std::optional<std::string> scene_id;
  std::optional<std::string> region_id;
  std::optional<std::string> entity_id;
  std::vector<double> bbox_norm;  // [x_min, y_min, x_max, y_max]
  std::vector<std::int64_t> bbox_px;  // [x_min, y_min, x_max, y_max]
  std::optional<double> orientation_deg;
  std::optional<std::string> text_direction; // ltr, rtl, ttb, btt, unknown
  std::optional<std::string> script;
  double confidence = 0.0;
  std::optional<std::string> foreground_color_observation_id;
  std::optional<std::string> background_color_observation_id;
  std::optional<double> contrast_ratio;
  std::optional<double> legibility_score;
  std::string provenance_id;
};

struct TextObservationRecord {
  std::string text_observation_id;
  std::string text_region_id;
  std::string observation_type = "text_recognition";
  std::string raw_text;
  std::string normalized_text;
  std::optional<nlohmann::json> language;
  double confidence = 0.0;
  std::optional<std::string> layout_class;
  std::vector<std::string> source_frame_ids;
  std::string provenance_id;
};

struct NumericValueRecord {
  std::string numeric_value_id;
  std::string text_observation_id;
  std::string text_region_id;
  std::string raw_text;
  std::string normalized_text;
  std::string number_kind = "unknown";
  std::string numeric_value;
  std::optional<std::string> unit;
  double confidence = 0.0;
  std::string parse_rule;
  std::string provenance_id;
};

struct TextAbsenceRecord {
  std::string schema_version = "svp-text-absence-v1";
  bool ocr_required = true;
  bool ocr_completed = false;
  std::int64_t text_region_count = 0;
  std::int64_t text_observation_count = 0;
  std::int64_t numeric_value_count = 0;
  std::string reason = "no_text_detected"; // no_text_detected, text_detected, ocr_completed, unsupported_media, processor_failed
  std::string provenance_id;
};

struct FoundationOcrStagingArtifact {
  std::vector<TextRegionRecord> text_regions;
  std::vector<TextObservationRecord> text_observations;
  std::vector<NumericValueRecord> numeric_values;
  TextAbsenceRecord text_absence;
  std::vector<nlohmann::json> processors;
  nlohmann::json manifest;
};

// Build the deterministic synthetic staging sample.
[[nodiscard]] FoundationOcrStagingArtifact build_foundation_ocr_staging_synthetic_artifact();

// Convert the staging artifact to JSON format.
[[nodiscard]] nlohmann::json foundation_ocr_staging_artifact_to_json(
    const FoundationOcrStagingArtifact& artifact);

// Build an honest staging artifact for real video media.
// When model_runtime_available is false, we set ocr_detection_run=false and
// ocr_recognition_run=false, populate text_absence reason as "processor_failed",
// and write empty record sets.
[[nodiscard]] FoundationOcrStagingArtifact build_real_ocr_staging_artifact(
    const media::MediaIngestPlan& plan,
    bool model_runtime_available);

[[nodiscard]] nlohmann::json text_region_to_json(const TextRegionRecord& record);
[[nodiscard]] nlohmann::json text_observation_to_json(const TextObservationRecord& record);
[[nodiscard]] nlohmann::json numeric_value_to_json(const NumericValueRecord& record);
[[nodiscard]] nlohmann::json text_absence_to_json(const TextAbsenceRecord& record);

}  // namespace svp::vision
