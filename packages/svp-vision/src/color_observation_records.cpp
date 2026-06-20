#include "svp/vision/color_observation_records.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace svp::vision {
namespace {

bool contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool registered_bucket_id(const std::string& bucket_id) {
  return contains(registered_color_bucket_ids(), bucket_id);
}

bool registered_target_type(const std::string& target_type) {
  static const std::vector<std::string> target_types = {
      "scene", "shot", "frame", "region", "entity", "text_region"};
  return contains(target_types, target_type);
}

bool registered_sampling_basis(const std::string& sampling_basis) {
  static const std::vector<std::string> sampling_basis_values = {
      "full_frame",
      "keyframe_full_frame",
      "mask",
      "region_crop",
      "entity_mask",
      "text_box",
      "text_foreground",
      "text_background",
  };
  return contains(sampling_basis_values, sampling_basis);
}

std::string make_observation_id(const std::string& prefix, const int number) {
  if (number < 0) {
    throw std::invalid_argument("color observation id number must be non-negative");
  }

  std::ostringstream stream;
  stream << prefix << std::setw(6) << std::setfill('0') << number;
  return stream.str();
}

void require_target_reference(
    const ColorObservationTarget& target,
    const ColorObservationTargetReferences& target_references) {
  const auto ids = target_references.ids_by_target_type.find(target.target_type);
  if (ids == target_references.ids_by_target_type.end()) {
    throw std::invalid_argument("missing color observation target reference set: " +
                                target.target_type);
  }
  if (!contains(ids->second, target.target_id)) {
    throw std::invalid_argument("color observation target does not exist: " +
                                target.target_type + "/" + target.target_id);
  }
}

void validate_summary(const QuantizedColorObservation& summary,
                      const ColorObservationTargetReferences& target_references) {
  if (!registered_target_type(summary.target.target_type)) {
    throw std::invalid_argument(
        "color observation target type is not registered");
  }
  if (!registered_sampling_basis(summary.target.sampling_basis)) {
    throw std::invalid_argument(
        "color observation sampling basis is not registered");
  }
  require_target_reference(summary.target, target_references);

  if (summary.color_space != registered_color_space()) {
    throw std::invalid_argument("color observation uses unregistered color space");
  }
  if (summary.color_bucket_registry_version !=
      registered_color_bucket_version()) {
    throw std::invalid_argument(
        "color observation uses unregistered bucket registry version");
  }
  if (summary.bucket_coverage.empty()) {
    throw std::invalid_argument("color observation bucket coverage is empty");
  }
  if (!registered_bucket_id(summary.dominant_bucket)) {
    throw std::invalid_argument(
        "color observation dominant bucket is not registered");
  }

  double coverage_total = 0.0;
  double max_coverage = -1.0;
  for (const auto& [bucket_id, coverage] : summary.bucket_coverage) {
    if (!registered_bucket_id(bucket_id)) {
      throw std::invalid_argument("color observation bucket is not registered: " +
                                  bucket_id);
    }
    if (!std::isfinite(coverage) || coverage < 0.0 || coverage > 1.0) {
      throw std::invalid_argument(
          "color observation bucket coverage must be within [0, 1]");
    }
    coverage_total += coverage;
    max_coverage = std::max(max_coverage, coverage);
  }

  const double tolerance = registered_color_percentage_sum_tolerance();
  if (std::fabs(coverage_total - 1.0) > tolerance) {
    throw std::invalid_argument(
        "color observation bucket coverage must sum to one");
  }
  if (std::fabs(summary.coverage_total - coverage_total) > tolerance) {
    throw std::invalid_argument(
        "color observation coverage total must match bucket coverage");
  }

  const auto dominant = summary.bucket_coverage.find(summary.dominant_bucket);
  if (dominant == summary.bucket_coverage.end()) {
    throw std::invalid_argument(
        "color observation dominant bucket is missing from coverage");
  }
  if (dominant->second + tolerance < max_coverage) {
    throw std::invalid_argument(
        "color observation dominant bucket must be a maximum coverage bucket");
  }
  if (!std::isfinite(summary.quality_score) || summary.quality_score < 0.0 ||
      summary.quality_score > 1.0) {
    throw std::invalid_argument(
        "color observation quality score must be within [0, 1]");
  }
}

nlohmann::json make_color_summary(const std::size_t record_count,
                                  const std::string& provenance_id) {
  return {
      {"schema_version", "svp-color-summary-v1"},
      {"color_observation_count", record_count},
      {"color_space", registered_color_space()},
      {"color_bucket_registry_version", registered_color_bucket_version()},
      {"percentage_sum_tolerance",
       registered_color_percentage_sum_tolerance()},
      {"provenance_id", provenance_id},
  };
}

nlohmann::json make_color_absence(const std::size_t record_count,
                                  const std::string& provenance_id) {
  return {
      {"schema_version", "svp-color-absence-v1"},
      {"color_required", true},
      {"color_completed", true},
      {"color_observation_count", record_count},
      {"reason", record_count == 0 ? "processor_completed" : "color_observed"},
      {"provenance_id", provenance_id},
  };
}

}  // namespace

ColorObservationRecordPlan plan_color_observation_records(
    const std::vector<QuantizedColorObservation>& summaries,
    const ColorObservationTargetReferences& target_references,
    const ColorObservationConstructionOptions& options) {
  if (options.provenance_id.empty()) {
    throw std::invalid_argument(
        "color observation construction requires provenance id");
  }
  if (options.observation_id_prefix.empty()) {
    throw std::invalid_argument(
        "color observation construction requires id prefix");
  }
  if (options.observation_id_prefix != "color_obs_") {
    throw std::invalid_argument(
        "color observation id prefix must match the RC2 schema");
  }

  ColorObservationRecordPlan plan;
  plan.records.reserve(summaries.size());

  int next_observation_number = options.first_observation_number;
  for (const QuantizedColorObservation& summary : summaries) {
    validate_summary(summary, target_references);
    plan.records.push_back(ColorObservationRecord{
        make_observation_id(options.observation_id_prefix,
                            next_observation_number),
        summary.target,
        summary.color_space,
        summary.color_bucket_registry_version,
        summary.bucket_coverage,
        summary.dominant_bucket,
        summary.coverage_total,
        summary.quality_score,
        options.provenance_id,
    });
    ++next_observation_number;
  }

  plan.color_summary = make_color_summary(plan.records.size(), options.provenance_id);
  plan.color_absence = make_color_absence(plan.records.size(), options.provenance_id);
  return plan;
}

nlohmann::json color_observation_record_to_json(
    const ColorObservationRecord& record) {
  return {
      {"color_observation_id", record.color_observation_id},
      {"target_type", record.target.target_type},
      {"target_id", record.target.target_id},
      {"start_us", record.target.start_us},
      {"end_us", record.target.end_us},
      {"frame_ids", record.target.frame_ids},
      {"sampling_basis", record.target.sampling_basis},
      {"color_space", record.color_space},
      {"color_bucket_registry_version", record.color_bucket_registry_version},
      {"bucket_coverage", record.bucket_coverage},
      {"dominant_bucket", record.dominant_bucket},
      {"coverage_total", record.coverage_total},
      {"quality_score", record.quality_score},
      {"provenance_id", record.provenance_id},
  };
}

nlohmann::json color_observation_records_to_jsonl_array(
    const std::vector<ColorObservationRecord>& records) {
  nlohmann::json json = nlohmann::json::array();
  for (const ColorObservationRecord& record : records) {
    json.push_back(color_observation_record_to_json(record));
  }
  return json;
}

}  // namespace svp::vision
