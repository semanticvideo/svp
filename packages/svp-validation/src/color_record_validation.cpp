#include "color_record_validation.hpp"

#include "json_schema_subset.hpp"
#include "record_file_reader.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <string_view>

namespace svp::validation {
namespace {

std::string package_entry_path(std::string_view entry) {
  return "/" + std::string{entry};
}

std::string record_path(std::string_view entry, std::size_t line) {
  return package_entry_path(entry) + ":" + std::to_string(line);
}

std::string schema_issue_message(const JsonSchemaIssue& issue) {
  return issue.path + ": " + issue.message;
}

void add_schema_findings(ValidationReport& report,
                         const ValidationCodeRegistry& registry,
                         const JsonLineRecord& record,
                         const nlohmann::json& schema) {
  for (const auto& issue : validate_json_schema_subset(record.value, schema)) {
    add_finding(report, make_finding(registry, kCodeColorInvalidObservationRecord,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     schema_issue_message(issue)));
  }
}

bool finite_number(const nlohmann::json& value) {
  return value.is_number() && std::isfinite(value.get<double>());
}

void validate_color_space(ValidationReport& report,
                          const ValidationCodeRegistry& registry,
                          const JsonLineRecord& record,
                          const OcrColorSpec& spec) {
  if (!record.value.contains("color_space") || !record.value.at("color_space").is_string()) {
    return;
  }

  const auto color_space = record.value.at("color_space").get<std::string>();
  if (!spec.color_space_ids.contains(color_space)) {
    add_finding(report, make_finding(registry, kCodeColorInvalidColorSpace,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     "color_space is not registered."));
  }
}

void validate_bucket_registry_version(ValidationReport& report,
                                      const ValidationCodeRegistry& registry,
                                      const JsonLineRecord& record,
                                      const OcrColorSpec& spec) {
  if (!record.value.contains("color_bucket_registry_version") ||
      !record.value.at("color_bucket_registry_version").is_string()) {
    return;
  }

  const auto registry_version =
      record.value.at("color_bucket_registry_version").get<std::string>();
  if (registry_version != spec.color_bucket_registry_version) {
    add_finding(report, make_finding(registry, kTempCodeColorInvalidBucketRegistryVersion,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     "color_bucket_registry_version is not registered."));
  }
}

void validate_bucket_coverage(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const JsonLineRecord& record,
                              const OcrColorSpec& spec) {
  if (!record.value.contains("bucket_coverage") ||
      !record.value.at("bucket_coverage").is_object()) {
    return;
  }

  double total = 0.0;
  bool saw_bound_error = false;
  const auto& coverage = record.value.at("bucket_coverage");
  for (const auto& [bucket_id, percentage] : coverage.items()) {
    if (!spec.color_bucket_ids.contains(bucket_id)) {
      add_finding(report, make_finding(registry, kCodeColorInvalidBucketId,
                                       record_path("colors/color_observations.jsonl",
                                                   record.line),
                                       "bucket_coverage contains an unregistered bucket ID."));
    }

    if (!finite_number(percentage) || percentage.get<double>() < 0.0 ||
        percentage.get<double>() > 1.0) {
      saw_bound_error = true;
      continue;
    }
    total += percentage.get<double>();
  }

  if (saw_bound_error) {
    add_finding(report, make_finding(registry, kCodeColorInvalidPercentageBounds,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     "bucket_coverage contains a non-finite or out-of-range percentage."));
  }

  if (std::abs(total - 1.0) > spec.percentage_sum_tolerance) {
    add_finding(report, make_finding(registry, kCodeColorInvalidPercentageTotal,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     "bucket_coverage percentages do not sum to 1.0 within tolerance."));
  }

  if (record.value.contains("coverage_total") && finite_number(record.value.at("coverage_total")) &&
      std::abs(record.value.at("coverage_total").get<double>() - total) >
          spec.percentage_sum_tolerance) {
    add_finding(report, make_finding(registry, kCodeColorInvalidPercentageTotal,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     "coverage_total does not match bucket_coverage within tolerance."));
  }
}

void validate_dominant_bucket(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const JsonLineRecord& record,
                              const OcrColorSpec& spec) {
  if (!record.value.contains("dominant_bucket") ||
      !record.value.at("dominant_bucket").is_string() ||
      !record.value.contains("bucket_coverage") ||
      !record.value.at("bucket_coverage").is_object()) {
    return;
  }

  const auto dominant = record.value.at("dominant_bucket").get<std::string>();
  const auto& coverage = record.value.at("bucket_coverage");
  if (!coverage.contains(dominant) || !finite_number(coverage.at(dominant))) {
    add_finding(report, make_finding(registry, kCodeColorInvalidDominantBucket,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     "dominant_bucket is not present in bucket_coverage."));
    return;
  }

  double maximum = 0.0;
  bool have_maximum = false;
  for (const auto& percentage : coverage) {
    if (!finite_number(percentage)) {
      continue;
    }
    const auto value = percentage.get<double>();
    if (!have_maximum || value > maximum) {
      maximum = value;
      have_maximum = true;
    }
  }

  if (!have_maximum) {
    return;
  }

  const auto dominant_value = coverage.at(dominant).get<double>();
  if (maximum - dominant_value > spec.percentage_sum_tolerance) {
    add_finding(report, make_finding(registry, kCodeColorInvalidDominantBucket,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     "dominant_bucket is not a maximum-coverage bucket within tolerance."));
  }
}

void validate_sampling_basis(ValidationReport& report,
                             const ValidationCodeRegistry& registry,
                             const JsonLineRecord& record,
                             const OcrColorSpec& spec) {
  if (!record.value.contains("sampling_basis") ||
      !record.value.at("sampling_basis").is_string()) {
    return;
  }

  const auto sampling_basis = record.value.at("sampling_basis").get<std::string>();
  if (!spec.color_sampling_basis_ids.contains(sampling_basis)) {
    add_finding(report, make_finding(registry, kCodeColorInvalidSamplingBasis,
                                     record_path("colors/color_observations.jsonl",
                                                 record.line),
                                     "sampling_basis is not registered."));
  }
}

}  // namespace

void add_color_record_findings(ValidationReport& report,
                               const ValidationCodeRegistry& registry,
                               const std::filesystem::path& package_path,
                               const svp::package::PackageLayout& layout,
                               const OcrColorSpec& spec) {
  constexpr std::string_view entry = "colors/color_observations.jsonl";
  if (!layout.has_entry(std::string{entry})) {
    return;
  }

  const auto records = read_json_lines_from_package(package_path, std::string{entry});
  if (!records.has_value()) {
    add_finding(report, make_finding(registry, kCodeColorInvalidObservationRecord,
                                     package_entry_path(entry), records.error_message));
    return;
  }

  for (const auto& record : records.records) {
    add_schema_findings(report, registry, record, spec.color_observation_schema);
    validate_color_space(report, registry, record, spec);
    validate_bucket_registry_version(report, registry, record, spec);
    validate_sampling_basis(report, registry, record, spec);
    validate_bucket_coverage(report, registry, record, spec);
    validate_dominant_bucket(report, registry, record, spec);
  }
}

}  // namespace svp::validation
