#include "loudness_record_validation.hpp"

#include "json_schema_subset.hpp"
#include "record_file_reader.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace svp::validation {
namespace {

constexpr std::string_view kLoudnessEntry = "media/audio/loudness.jsonl";
constexpr std::string_view kLoudnessSummaryEntry = "media/audio/loudness_summary.json";
constexpr std::string_view kAudioAbsenceEntry = "media/audio/audio_absence.json";

std::string package_entry_path(std::string_view entry) {
  return "/" + std::string{entry};
}

std::string record_path(std::string_view entry, std::size_t line) {
  return package_entry_path(entry) + ":" + std::to_string(line);
}

std::string schema_issue_message(const JsonSchemaIssue& issue) {
  return issue.path + ": " + issue.message;
}

bool number_or_null_finite(const nlohmann::json& value) {
  return value.is_null() || (value.is_number() && std::isfinite(value.get<double>()));
}

std::unordered_set<std::string> known_stream_ids(
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout) {
  std::unordered_set<std::string> ids;
  if (!layout.has_entry(std::string{kAudioAbsenceEntry})) {
    return ids;
  }
  const auto absence = read_json_from_package(package_path,
                                              std::string{kAudioAbsenceEntry});
  if (!absence.has_value() || !absence.value.is_object()) {
    return ids;
  }
  const auto it = absence.value.find("original_audio_stream_ids");
  if (it == absence.value.end() || !it->is_array()) {
    return ids;
  }
  for (const nlohmann::json& id : *it) {
    if (id.is_string()) {
      ids.insert(id.get<std::string>());
    }
  }
  return ids;
}

void validate_loudness_value_fields(ValidationReport& report,
                                    const ValidationCodeRegistry& registry,
                                    const JsonLineRecord& record) {
  static constexpr std::string_view kValueFields[] = {
      "momentary_lufs",
      "shortterm_lufs",
      "true_peak_dbtp",
  };
  for (const std::string_view field : kValueFields) {
    const auto it = record.value.find(std::string{field});
    if (it == record.value.end() || !number_or_null_finite(*it)) {
      add_finding(report, make_finding(registry, kCodeMediaInvalidLoudnessValue,
                                     record_path(kLoudnessEntry, record.line),
                                     std::string{field} +
                                         " is neither a finite number nor null."));
    }
  }
}

void validate_loudness_summary(ValidationReport& report,
                               const ValidationCodeRegistry& registry,
                               const std::filesystem::path& package_path,
                               const svp::package::PackageLayout& layout,
                               const nlohmann::json& summary_schema,
                               const std::unordered_set<std::string>& stream_ids) {
  if (!layout.has_entry(std::string{kLoudnessSummaryEntry})) {
    return;
  }

  const auto summary = read_json_from_package(package_path,
                                              std::string{kLoudnessSummaryEntry});
  if (!summary.has_value()) {
    add_finding(report, make_finding(registry, kCodeMediaInvalidLoudnessSummary,
                                     package_entry_path(kLoudnessSummaryEntry),
                                     summary.error_message));
    return;
  }

  for (const auto& issue :
       validate_json_schema_subset(summary.value, summary_schema)) {
    add_finding(report, make_finding(registry, kCodeMediaInvalidLoudnessSummary,
                                     package_entry_path(kLoudnessSummaryEntry),
                                     schema_issue_message(issue)));
  }

  const auto streams_it = summary.value.find("streams");
  if (streams_it == summary.value.end() || !streams_it->is_array()) {
    return;
  }
  for (const nlohmann::json& stream : *streams_it) {
    if (!stream.is_object()) {
      continue;
    }
    const auto id_it = stream.find("target_id");
    if (id_it != stream.end() && id_it->is_string() &&
        !stream_ids.empty() && !stream_ids.contains(id_it->get<std::string>())) {
      add_finding(report, make_finding(registry, kCodeMediaInvalidLoudnessTarget,
                                     package_entry_path(kLoudnessSummaryEntry),
                                     "stream target_id does not resolve to a recorded audio stream."));
    }
  }
}

}  // namespace

void add_loudness_record_findings(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                const std::filesystem::path& package_path,
                                const svp::package::PackageLayout& layout,
                                const nlohmann::json& loudness_observation_schema,
                                const nlohmann::json& loudness_summary_schema) {
  const std::unordered_set<std::string> stream_ids =
      known_stream_ids(package_path, layout);

  if (layout.has_entry(std::string{kLoudnessEntry})) {
    const auto records =
        read_json_lines_from_package(package_path, std::string{kLoudnessEntry});
    if (!records.has_value()) {
      add_finding(report, make_finding(registry, kCodeMediaInvalidLoudnessRecord,
                                       package_entry_path(kLoudnessEntry),
                                       records.error_message));
    } else {
      std::unordered_map<std::string, std::int64_t> last_end_us_per_target;
      for (const auto& record : records.records) {
        for (const auto& issue :
             validate_json_schema_subset(record.value, loudness_observation_schema)) {
          add_finding(report,
                      make_finding(registry, kCodeMediaInvalidLoudnessRecord,
                                   record_path(kLoudnessEntry, record.line),
                                   schema_issue_message(issue)));
        }

        validate_loudness_value_fields(report, registry, record);

        const auto start_it = record.value.find("start_us");
        const auto end_it = record.value.find("end_us");
        const auto target_it = record.value.find("target_id");
        if (start_it != record.value.end() && start_it->is_number_integer() &&
            end_it != record.value.end() && end_it->is_number_integer()) {
          const std::int64_t start_us = start_it->get<std::int64_t>();
          const std::int64_t end_us = end_it->get<std::int64_t>();
          if (start_us < 0 || end_us <= start_us) {
            add_finding(report,
                        make_finding(registry, kCodeMediaInvalidLoudnessTiming,
                                     record_path(kLoudnessEntry, record.line),
                                     "record has start_us >= end_us or a negative timestamp."));
          }
          const std::string target_key =
              target_it != record.value.end() && target_it->is_string()
                  ? target_it->get<std::string>()
                  : std::string{};
          const auto last_it = last_end_us_per_target.find(target_key);
          if (last_it != last_end_us_per_target.end() &&
              start_us < last_it->second) {
            add_finding(report,
                        make_finding(registry, kCodeMediaInvalidLoudnessTiming,
                                     record_path(kLoudnessEntry, record.line),
                                     "record overlaps or reorders windows for its target_id."));
          }
          last_end_us_per_target[target_key] =
              std::max(last_end_us_per_target[target_key], end_us);
        }

        if (target_it != record.value.end() && target_it->is_string() &&
            !stream_ids.empty() &&
            !stream_ids.contains(target_it->get<std::string>())) {
          add_finding(report,
                      make_finding(registry, kCodeMediaInvalidLoudnessTarget,
                                   record_path(kLoudnessEntry, record.line),
                                   "target_id does not resolve to a recorded audio stream."));
        }
      }
    }
  }

  validate_loudness_summary(report, registry, package_path, layout,
                            loudness_summary_schema, stream_ids);
}

}  // namespace svp::validation
