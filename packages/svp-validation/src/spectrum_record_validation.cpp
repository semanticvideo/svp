#include "spectrum_record_validation.hpp"

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

constexpr std::string_view kSpectrumEntry = "media/audio/spectrum.jsonl";
constexpr std::string_view kSpectrumSummaryEntry = "media/audio/spectrum_summary.json";
constexpr std::string_view kAudioAbsenceEntry = "media/audio/audio_absence.json";

// The band table is fixed by the package schema at ten IEC 61260 octave bands.
constexpr std::size_t kSpectrumBandCount = 10;

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

void validate_spectrum_bands(ValidationReport& report,
                             const ValidationCodeRegistry& registry,
                             const JsonLineRecord& record) {
  const auto it = record.value.find("bands");
  if (it == record.value.end() || !it->is_array()) {
    return;
  }
  if (it->size() != kSpectrumBandCount) {
    add_finding(report, make_finding(registry, kCodeMediaInvalidSpectrumValue,
                                     record_path(kSpectrumEntry, record.line),
                                     "bands must contain exactly " +
                                         std::to_string(kSpectrumBandCount) +
                                         " entries."));
    return;
  }
  for (std::size_t index = 0; index < it->size(); ++index) {
    const nlohmann::json& value = (*it)[index];
    if (!number_or_null_finite(value)) {
      add_finding(report, make_finding(registry, kCodeMediaInvalidSpectrumValue,
                                       record_path(kSpectrumEntry, record.line),
                                       "bands[" + std::to_string(index) +
                                           "] is neither a finite number nor null."));
    }
  }
}

void validate_spectrum_summary(ValidationReport& report,
                               const ValidationCodeRegistry& registry,
                               const std::filesystem::path& package_path,
                               const svp::package::PackageLayout& layout,
                               const nlohmann::json& summary_schema,
                               const std::unordered_set<std::string>& stream_ids) {
  if (!layout.has_entry(std::string{kSpectrumSummaryEntry})) {
    return;
  }

  const auto summary = read_json_from_package(package_path,
                                              std::string{kSpectrumSummaryEntry});
  if (!summary.has_value()) {
    add_finding(report, make_finding(registry, kCodeMediaInvalidSpectrumSummary,
                                     package_entry_path(kSpectrumSummaryEntry),
                                     summary.error_message));
    return;
  }

  for (const auto& issue :
       validate_json_schema_subset(summary.value, summary_schema)) {
    add_finding(report, make_finding(registry, kCodeMediaInvalidSpectrumSummary,
                                     package_entry_path(kSpectrumSummaryEntry),
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
      add_finding(report, make_finding(registry, kCodeMediaInvalidSpectrumTarget,
                                     package_entry_path(kSpectrumSummaryEntry),
                                     "stream target_id does not resolve to a recorded audio stream."));
    }
  }
}

}  // namespace

void add_spectrum_record_findings(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                const std::filesystem::path& package_path,
                                const svp::package::PackageLayout& layout,
                                const nlohmann::json& spectrum_observation_schema,
                                const nlohmann::json& spectrum_summary_schema) {
  const std::unordered_set<std::string> stream_ids =
      known_stream_ids(package_path, layout);

  if (layout.has_entry(std::string{kSpectrumEntry})) {
    const auto records =
        read_json_lines_from_package(package_path, std::string{kSpectrumEntry});
    if (!records.has_value()) {
      add_finding(report, make_finding(registry, kCodeMediaInvalidSpectrumRecord,
                                       package_entry_path(kSpectrumEntry),
                                       records.error_message));
    } else {
      std::unordered_map<std::string, std::int64_t> last_end_us_per_target;
      for (const auto& record : records.records) {
        for (const auto& issue :
             validate_json_schema_subset(record.value, spectrum_observation_schema)) {
          add_finding(report,
                      make_finding(registry, kCodeMediaInvalidSpectrumRecord,
                                   record_path(kSpectrumEntry, record.line),
                                   schema_issue_message(issue)));
        }

        validate_spectrum_bands(report, registry, record);

        const auto start_it = record.value.find("start_us");
        const auto end_it = record.value.find("end_us");
        const auto target_it = record.value.find("target_id");
        if (start_it != record.value.end() && start_it->is_number_integer() &&
            end_it != record.value.end() && end_it->is_number_integer()) {
          const std::int64_t start_us = start_it->get<std::int64_t>();
          const std::int64_t end_us = end_it->get<std::int64_t>();
          if (start_us < 0 || end_us <= start_us) {
            add_finding(report,
                        make_finding(registry, kCodeMediaInvalidSpectrumTiming,
                                     record_path(kSpectrumEntry, record.line),
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
                        make_finding(registry, kCodeMediaInvalidSpectrumTiming,
                                     record_path(kSpectrumEntry, record.line),
                                     "record overlaps or reorders windows for its target_id."));
          }
          last_end_us_per_target[target_key] =
              std::max(last_end_us_per_target[target_key], end_us);
        }

        if (target_it != record.value.end() && target_it->is_string() &&
            !stream_ids.empty() &&
            !stream_ids.contains(target_it->get<std::string>())) {
          add_finding(report,
                      make_finding(registry, kCodeMediaInvalidSpectrumTarget,
                                   record_path(kSpectrumEntry, record.line),
                                   "target_id does not resolve to a recorded audio stream."));
        }
      }
    }
  }

  validate_spectrum_summary(report, registry, package_path, layout,
                            spectrum_summary_schema, stream_ids);
}

}  // namespace svp::validation
