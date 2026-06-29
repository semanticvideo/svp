#include "svp/core/version.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_summary.hpp"
#include "svp/query/query_ops.hpp"
#include "svp/query/query_reader.hpp"
#include "svp/query/traversal.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string yes_no(bool value) {
  return value ? "yes" : "no";
}

std::string value_or_unknown(const std::string& value) {
  return value.empty() ? "unknown" : value;
}

std::string count_or_unknown(const svp::package::PackageJsonlCountSummary& count) {
  if (!count.present || !count.readable) {
    return "unknown";
  }
  return std::to_string(count.record_count);
}

std::string count_match_label(const svp::package::PackageJsonlCountSummary& count,
                              const std::string& declared_count) {
  if (!count.present || !count.readable || declared_count.empty()) {
    return "unknown";
  }
  return declared_count == std::to_string(count.record_count) ? "yes" : "no";
}

std::string area_label(svp::package::PackageLayoutRequirementArea area) {
  switch (area) {
    case svp::package::PackageLayoutRequirementArea::core:
      return "core";
    case svp::package::PackageLayoutRequirementArea::text:
      return "text";
    case svp::package::PackageLayoutRequirementArea::colors:
      return "colors";
  }
  return "unknown";
}

struct DumpTarget {
  std::string_view section;
  std::string_view entry;
};

const std::vector<DumpTarget>& dump_targets() {
  static const std::vector<DumpTarget> targets{
      {"manifest", "manifest.json"},
      {"index_manifest", "index/index_manifest.json"},
  };
  return targets;
}

const DumpTarget* find_dump_target(std::string_view section) {
  for (const auto& target : dump_targets()) {
    if (target.section == section) {
      return &target;
    }
  }
  return nullptr;
}

std::uint64_t count_present_required_items(const svp::package::PackageSummary& summary) {
  return static_cast<std::uint64_t>(
      std::ranges::count_if(summary.required_items,
                            [](const svp::package::PackageRequiredItemSummary& item) {
                              return item.present;
                            }));
}

void print_json_file_status(std::string_view label,
                            const svp::package::PackageJsonFileSummary& file) {
  std::cout << "  " << label << ": " << yes_no(file.present);
  if (file.present && !file.readable) {
    std::cout << " (unreadable: " << file.error_message << ")";
  } else if (file.readable && !file.parsed) {
    std::cout << " (invalid JSON: " << file.error_message << ")";
  }
  std::cout << "\n";
}

void print_jsonl_count(std::string_view label,
                       const svp::package::PackageJsonlCountSummary& count) {
  std::cout << "  " << label << ": ";
  if (!count.present) {
    std::cout << "missing\n";
    return;
  }
  if (!count.readable) {
    std::cout << "unreadable (" << count.error_message << ")\n";
    return;
  }
  std::cout << count.record_count << "\n";
}

void print_manifest(const svp::package::PackageSummary& summary) {
  std::cout << "Manifest\n";
  print_json_file_status("manifest.json", summary.manifest.file);
  if (!summary.manifest.file.parsed) {
    return;
  }

  std::cout << "  svp_version: " << value_or_unknown(summary.manifest.svp_version) << "\n";
  std::cout << "  package_id: " << value_or_unknown(summary.manifest.package_id) << "\n";
  std::cout << "  created_utc: " << value_or_unknown(summary.manifest.created_utc) << "\n";
  std::cout << "  primary_media_id: "
            << value_or_unknown(summary.manifest.primary_media_id) << "\n";
  std::cout << "  timebase: " << value_or_unknown(summary.manifest.timebase_unit);
  if (!summary.manifest.timebase_origin.empty()) {
    std::cout << " from " << summary.manifest.timebase_origin;
  }
  std::cout << "\n";
  std::cout << "  canonical_raster: "
            << value_or_unknown(summary.manifest.canonical_raster) << "\n";
}

void print_required_layout(const svp::package::PackageSummary& summary) {
  const auto present_count = count_present_required_items(summary);

  std::cout << "Required layout\n";
  std::cout << "  present: " << present_count << "/" << summary.required_items.size() << "\n";

  bool printed_missing_header = false;
  for (const auto& item : summary.required_items) {
    if (item.present) {
      continue;
    }

    if (!printed_missing_header) {
      std::cout << "  missing:\n";
      printed_missing_header = true;
    }

    std::cout << "    - " << item.path << " (" << area_label(item.area) << ")\n";
  }

  if (!printed_missing_header) {
    std::cout << "  missing: none\n";
  }

  if (!summary.invalid_entry_paths.empty()) {
    std::cout << "  invalid entry paths:\n";
    for (const auto& path : summary.invalid_entry_paths) {
      std::cout << "    - " << path << "\n";
    }
  }
}

void print_text(const svp::package::PackageSummary& summary) {
  std::cout << "Text\n";
  print_jsonl_count("text_regions", summary.text.text_regions);
  print_jsonl_count("text_observations", summary.text.text_observations);
  print_jsonl_count("numeric_values", summary.text.numeric_values);
  print_json_file_status("text_absence.json", summary.text.absence);
  if (summary.text.absence.parsed) {
    std::cout << "  ocr_required: " << value_or_unknown(summary.text.ocr_required) << "\n";
    std::cout << "  ocr_completed: " << value_or_unknown(summary.text.ocr_completed) << "\n";
    std::cout << "  absence_counts: regions="
              << value_or_unknown(summary.text.text_region_count)
              << " observations=" << value_or_unknown(summary.text.text_observation_count)
              << " numeric_values=" << value_or_unknown(summary.text.numeric_value_count)
              << "\n";
    std::cout << "  file_counts_match_absence: regions="
              << count_match_label(summary.text.text_regions, summary.text.text_region_count)
              << " observations="
              << count_match_label(summary.text.text_observations,
                                   summary.text.text_observation_count)
              << " numeric_values="
              << count_match_label(summary.text.numeric_values, summary.text.numeric_value_count)
              << "\n";
    std::cout << "  reason: " << value_or_unknown(summary.text.absence_reason) << "\n";
  }
}

void print_colors(const svp::package::PackageSummary& summary) {
  std::cout << "Colors\n";
  print_jsonl_count("color_observations", summary.colors.color_observations);
  print_json_file_status("color_summary.json", summary.colors.summary);
  if (summary.colors.summary.parsed) {
    std::cout << "  summary_count: "
              << value_or_unknown(summary.colors.color_observation_count) << "\n";
    std::cout << "  file_count: "
              << count_or_unknown(summary.colors.color_observations) << "\n";
    std::cout << "  file_count_matches_summary: "
              << count_match_label(summary.colors.color_observations,
                                   summary.colors.color_observation_count)
              << "\n";
    std::cout << "  color_space: " << value_or_unknown(summary.colors.color_space) << "\n";
    std::cout << "  bucket_registry: "
              << value_or_unknown(summary.colors.color_bucket_registry_version) << "\n";
  }
  print_json_file_status("color_absence.json", summary.colors.absence);
  if (summary.colors.absence.parsed) {
    std::cout << "  color_required: " << value_or_unknown(summary.colors.color_required) << "\n";
    std::cout << "  color_completed: " << value_or_unknown(summary.colors.color_completed)
              << "\n";
    std::cout << "  reason: " << value_or_unknown(summary.colors.absence_reason) << "\n";
  }
}

void print_index(const svp::package::PackageSummary& summary) {
  std::cout << "Index\n";
  std::cout << "  index.sqlite: " << yes_no(summary.index.sqlite_present) << "\n";
  print_json_file_status("index_manifest.json", summary.index.file);
  if (!summary.index.file.parsed) {
    return;
  }

  std::cout << "  schema_version: " << value_or_unknown(summary.index.file.schema_version)
            << "\n";
  std::cout << "  index_schema_version: "
            << value_or_unknown(summary.index.index_schema_version) << "\n";
  std::cout << "  sqlite_file: " << value_or_unknown(summary.index.sqlite_file) << "\n";
  std::cout << "  logical_row_stream_version: "
            << value_or_unknown(summary.index.logical_row_stream_version) << "\n";
  std::cout << "  table_count: " << value_or_unknown(summary.index.table_count) << "\n";
  std::cout << "  row_count: " << value_or_unknown(summary.index.row_count) << "\n";
  std::cout << "  created_from: " << yes_no(summary.index.has_created_from) << "\n";
}

void print_validation_report(const svp::package::PackageSummary& summary) {
  std::cout << "Validation report\n";
  print_json_file_status("provenance/validation.json", summary.validation_report.file);
  if (!summary.validation_report.file.parsed) {
    return;
  }

  std::cout << "  status: " << value_or_unknown(summary.validation_report.status) << "\n";
  if (!summary.validation_report.core_status.empty()) {
    std::cout << "  core_status: " << summary.validation_report.core_status << "\n";
  }
  if (!summary.validation_report.authenticity_status.empty()) {
    std::cout << "  authenticity_status: " << summary.validation_report.authenticity_status
              << "\n";
  }
}

void print_summary(const svp::package::PackageSummary& summary) {
  std::cout << "SVP package summary\n";
  std::cout << "Path: " << summary.probe.path.string() << "\n";
  std::cout << "Exists: " << yes_no(summary.probe.exists) << "\n";
  std::cout << "Regular file: " << yes_no(summary.probe.is_regular_file) << "\n";
  std::cout << "SVP extension: " << yes_no(summary.probe.has_svp_extension) << "\n";
  std::cout << "Readable ZIP layout: " << yes_no(summary.layout_readable) << "\n";
  if (!summary.layout_readable) {
    std::cout << "Layout error: " << summary.layout_error_message << "\n";
    return;
  }

  std::cout << "Entries: " << summary.entry_count << "\n";
  std::cout << "Root entries:";
  for (const auto& root_entry : summary.root_entries) {
    std::cout << " " << root_entry;
  }
  std::cout << "\n\n";

  print_manifest(summary);
  std::cout << "\n";
  print_required_layout(summary);
  std::cout << "\n";
  print_text(summary);
  std::cout << "\n";
  print_colors(summary);
  std::cout << "\n";
  print_index(summary);
  std::cout << "\n";
  print_validation_report(summary);
}

int dump_json_entry(const std::filesystem::path& package_path, const DumpTarget& target) {
  const auto read_result = svp::package::read_package_entry(
      package_path, std::string{target.entry});
  if (!read_result.has_value()) {
    std::cerr << "Unable to read " << target.entry << ": "
              << read_result.error_message() << "\n";
    return 1;
  }

  const auto parsed = nlohmann::json::parse(read_result.value(), nullptr, false);
  if (parsed.is_discarded()) {
    std::cerr << "Unable to parse " << target.entry << " as JSON\n";
    return 1;
  }

  std::cout << parsed.dump(2) << "\n";
  return 0;
}

int dump_sections(const std::filesystem::path& package_path, std::string_view section) {
  if (section == "all") {
    int status = 0;
    bool first = true;
    for (const auto& target : dump_targets()) {
      if (!first) {
        std::cout << "\n";
      }
      first = false;
      std::cout << "# " << target.entry << "\n";
      status = std::max(status, dump_json_entry(package_path, target));
    }
    return status;
  }

  const auto* target = find_dump_target(section);
  if (target == nullptr) {
    std::cerr << "Unsupported dump section: " << section
              << " (expected manifest, index_manifest, or all)\n";
    return 2;
  }

  return dump_json_entry(package_path, *target);
}

}  // namespace

namespace query_cmd {

nlohmann::json json_string_value(const nlohmann::json& j, std::string_view key) {
  if (!j.is_object()) {
    return {};
  }
  const auto it = j.find(key);
  if (it == j.end()) {
    return {};
  }
  return *it;
}

void print_layers(const std::filesystem::path& package_path, bool json_output) {
  const auto summary = svp::query::list_layers(package_path);

  if (json_output) {
    nlohmann::json layers = nlohmann::json::array();
    for (const auto& layer : summary.layers) {
      nlohmann::json entry = {
          {"section", layer.section},
          {"entry", layer.entry},
          {"kind", layer.kind},
          {"present", layer.present},
          {"record_count", layer.record_count}
      };
      if (layer.has_malformed) {
        entry["has_malformed"] = true;
        entry["malformed_line_count"] = layer.malformed_line_count;
        entry["error"] = layer.error_message;
      }
      layers.push_back(entry);
    }
    std::cout << nlohmann::json{
        {"total_entries", summary.total_entries},
        {"layers", layers}
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Package layers (" << summary.total_entries << " present)\n";
  for (const auto& layer : summary.layers) {
    std::cout << "  [" << layer.section << "] " << layer.entry;
    if (layer.present) {
      std::cout << " (" << layer.record_count << " records)";
    } else {
      std::cout << " (missing)";
    }
    if (layer.has_malformed) {
      std::cout << " MALFORMED";
      if (layer.malformed_line_count > 0) {
        std::cout << " (" << layer.malformed_line_count << " bad lines)";
      }
    }
    std::cout << "\n";
  }
}

void print_transcript(const std::filesystem::path& package_path, bool json_output) {
  const auto result = svp::query::transcript_summary(package_path);

  if (!result.present) {
    if (json_output) {
      std::cout << R"({"present":false})" << "\n";
    } else {
      std::cout << "Transcript: not present\n";
    }
    return;
  }

  if (!result.parsed) {
    if (json_output) {
      std::cout << nlohmann::json{
          {"present", true},
          {"parsed", false},
          {"error", result.error_message}
      }.dump(2) << "\n";
    } else {
      std::cout << "Transcript: present but JSON parse failed: "
                << result.error_message << "\n";
    }
    return;
  }

  if (json_output) {
    nlohmann::json out = result.transcript_json;
    out["word_count_file"] = result.word_count_file;
    out["speaker_count_file"] = result.speaker_count_file;
    out["speech_region_count_file"] = result.speech_region_count;
    std::cout << out.dump(2) << "\n";
    return;
  }

  std::cout << "Transcript summary\n";
  const auto lang = result.transcript_json.value("language", nlohmann::json{});
  if (lang.is_object()) {
    std::cout << "  language: " << lang.value("primary", "unknown");
    std::cout << " (" << lang.value("mode", "unknown") << ")\n";
  } else {
    std::cout << "  language: not present\n";
  }
  std::cout << "  word_count (declared): " << result.transcript_json.value("word_count", 0) << "\n";
  std::cout << "  word_count (file): " << result.word_count_file << "\n";
  if (result.transcript_json.contains("speaker_count")) {
    std::cout << "  speaker_count (declared): " << result.transcript_json.value("speaker_count", 0) << "\n";
  }
  std::cout << "  speaker_count (file): " << result.speaker_count_file << "\n";
  std::cout << "  speech_regions (file): " << result.speech_region_count << "\n";
  if (result.transcript_json.contains("duration_us")) {
    std::cout << "  duration_us: " << result.transcript_json.value("duration_us", 0) << "\n";
  }

  if (result.word_count_file > 0) {
    const auto words = svp::query::read_jsonl_entry(package_path, "transcript/words.jsonl");
    if (words.readable && !words.records.empty()) {
      const auto& first = words.records.front();
      const auto& last = words.records.back();
      std::cout << "  word range: " << first.value("id", "?") << " to " << last.value("id", "?");
      std::cout << " (" << first.value("start_us", 0) << "us - " << last.value("end_us", 0) << "us)\n";
    }
  }
}

void print_find_words(const std::filesystem::path& package_path,
                      const std::string& search_text, std::size_t max_results,
                      bool json_output) {
  const auto matches = svp::query::find_words(package_path, search_text, max_results);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : matches) {
      arr.push_back(m.record);
    }
    std::cout << nlohmann::json{
        {"search_text", search_text},
        {"match_count", matches.size()},
        {"matches", arr}
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Found " << matches.size() << " word(s) matching \"" << search_text << "\"\n";
  for (const auto& m : matches) {
    std::cout << "  " << m.record.value("id", "?")
              << " text=\"" << m.record.value("text", "?") << "\""
              << " start_us=" << m.record.value("start_us", 0)
              << " end_us=" << m.record.value("end_us", 0)
              << " speaker=" << m.record.value("speaker_id", "?") << "\n";
  }
}

void print_speakers(const std::filesystem::path& package_path, bool json_output) {
  const auto speakers = svp::query::list_speakers(package_path);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : speakers) {
      nlohmann::json rec = s.record;
      rec["word_count"] = s.word_count;
      arr.push_back(rec);
    }
    std::cout << nlohmann::json{
        {"speaker_count", speakers.size()},
        {"speakers", arr}
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Speakers (" << speakers.size() << ")\n";
  for (const auto& s : speakers) {
    std::cout << "  " << s.record.value("id", "?")
              << " name=\"" << s.record.value("display_name", "?") << "\""
              << " words=" << s.word_count
              << " speech_us=" << s.record.value("total_speech_us", 0) << "\n";
  }
}

void print_ocr(const std::filesystem::path& package_path,
               const std::optional<std::string>& text_filter,
               std::size_t max_results, bool json_output) {
  const auto observations = svp::query::list_ocr_observations(
      package_path, text_filter, max_results);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& o : observations) {
      arr.push_back(o.record);
    }
    std::cout << nlohmann::json{
        {"observation_count", observations.size()},
        {"observations", arr}
    }.dump(2) << "\n";
    return;
  }

  std::cout << "OCR text observations (" << observations.size() << ")\n";
  for (const auto& o : observations) {
    std::cout << "  " << o.record.value("text_observation_id", "?")
              << " type=" << o.record.value("observation_type", "?")
              << " raw=\"" << o.record.value("raw_text", "") << "\""
              << " confidence=" << o.record.value("confidence", 0.0);

    const auto crop_refs = o.record.find("evidence_crop_refs");
    if (crop_refs != o.record.end() && crop_refs->is_array() && !crop_refs->empty()) {
      std::cout << " crops=[";
      bool first = true;
      for (const auto& ref : *crop_refs) {
        if (!first) std::cout << ", ";
        first = false;
        std::cout << ref.get<std::string>();
      }
      std::cout << "]";
    }
    std::cout << "\n";
  }

  if (!text_filter.has_value()) {
    const auto crops = svp::query::read_jsonl_entry(package_path, "text/evidence_crops.jsonl");
    if (crops.readable && !crops.records.empty()) {
      std::cout << "\nEvidence crops (" << crops.records.size() << ")\n";
      for (const auto& c : crops.records) {
        std::cout << "  " << c.value("crop_id", "?")
                  << " path=" << c.value("crop_file_path", "?")
                  << " size=" << c.value("crop_size_bytes", 0) << " bytes\n";
      }
    }
  }
}

void print_colors(const std::filesystem::path& package_path,
                  const std::optional<std::string>& dominant_filter,
                  std::optional<double> min_coverage,
                  std::size_t max_results, bool json_output) {
  const auto observations = svp::query::list_color_observations(
      package_path, dominant_filter, min_coverage, max_results);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& o : observations) {
      arr.push_back(o.record);
    }
    std::cout << nlohmann::json{
        {"observation_count", observations.size()},
        {"observations", arr}
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Color observations (" << observations.size() << ")\n";
  for (const auto& o : observations) {
    std::cout << "  " << o.record.value("color_observation_id", "?")
              << " target=" << o.record.value("target_type", "?")
              << ":" << o.record.value("target_id", "?")
              << " dominant=" << o.record.value("dominant_bucket", "?")
              << " coverage_total=" << o.record.value("coverage_total", 0.0);

    const auto buckets = o.record.find("bucket_coverage");
    if (buckets != o.record.end() && buckets->is_object()) {
      std::cout << " buckets={";
      bool first = true;
      for (auto it = buckets->begin(); it != buckets->end(); ++it) {
        if (!first) std::cout << ", ";
        first = false;
        std::cout << it.key() << "=" << it.value();
      }
      std::cout << "}";
    }
    std::cout << "\n";
  }
}

void print_validation(const std::filesystem::path& package_path, bool json_output) {
  const auto info = svp::query::show_validation(package_path);

  if (!info.present) {
    if (json_output) {
      std::cout << R"({"present":false})" << "\n";
    } else {
      std::cout << "Validation: not present\n";
    }
    return;
  }

  if (!info.parsed) {
    if (json_output) {
      std::cout << nlohmann::json{
          {"present", true},
          {"parsed", false},
          {"error", info.error_message}
      }.dump(2) << "\n";
    } else {
      std::cout << "Validation: present but JSON parse failed: "
                << info.error_message << "\n";
    }
    return;
  }

  if (json_output) {
    nlohmann::json out = {{"present", true}, {"parsed", true}};
    out["record"] = info.record;
    std::cout << out.dump(2) << "\n";
    return;
  }

  std::cout << "Validation status\n";
  std::cout << "  status: " << info.record.value("status", "unknown") << "\n";
  if (info.record.contains("core_status")) {
    std::cout << "  core_status: " << info.record.value("core_status", "unknown") << "\n";
  }
  if (info.record.contains("authenticity_status")) {
    std::cout << "  authenticity_status: " << info.record.value("authenticity_status", "unknown") << "\n";
  }
}

void print_relationships(const std::filesystem::path& package_path,
                          const std::optional<std::string>& class_filter,
                          std::size_t max_results, bool json_output) {
  const auto summary = svp::query::relationship_summary(package_path);

  if (!summary.present) {
    if (json_output) {
      std::cout << R"({"present":false})" << "\n";
    } else {
      std::cout << "Relationships: not present\n";
    }
    return;
  }

  if (!summary.readable) {
    if (json_output) {
      std::cout << nlohmann::json{
          {"present", true}, {"readable", false},
          {"error", summary.error_message}
      }.dump(2) << "\n";
    } else {
      std::cout << "Relationships: present but unreadable: "
                << summary.error_message << "\n";
    }
    return;
  }

  const auto rels = svp::query::list_relationships(package_path, class_filter, max_results);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : rels) {
      nlohmann::json entry = r.record;
      entry["_relationship_class"] = r.relationship_class;
      arr.push_back(entry);
    }
    std::cout << nlohmann::json{
        {"present", true},
        {"readable", true},
        {"total_count", summary.total_count},
        {"support_count", summary.support_count},
        {"semantic_count", summary.semantic_count},
        {"unknown_count", summary.unknown_count},
        {"returned_count", rels.size()},
        {"relationships", arr}
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Relationships (" << summary.total_count << " total)\n";
  std::cout << "  support: " << summary.support_count << "\n";
  std::cout << "  semantic: " << summary.semantic_count << "\n";
  std::cout << "  unknown: " << summary.unknown_count << "\n";
  std::cout << "  showing " << rels.size() << " (limit " << max_results << ")\n\n";

  for (const auto& r : rels) {
    const auto& rec = r.record;
    std::cout << "  " << rec.value("id", "?")
              << " [" << r.relationship_class << "] "
              << rec.value("type", "?")
              << "  " << rec.value("source_id", "?")
              << " -> " << rec.value("target_id", "?");
    if (rec.contains("confidence")) {
      std::cout << "  conf=" << rec.value("confidence", 0.0);
    }
    std::cout << "\n";
  }
}

std::string summary_kind(const nlohmann::json& summary) {
  if (!summary.is_object()) return "";
  return summary.value("kind", "");
}

std::string compact_node_summary(const nlohmann::json& summary) {
  if (!summary.is_object()) return "";
  const auto kind = summary.value("kind", "");
  if (kind == "word") {
    return "word text=\"" + summary.value("text", "?") + "\""
           + " start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "speaker") {
    return "speaker name=\"" + summary.value("display_name", "?") + "\"";
  }
  if (kind == "speaker_segment") {
    return "segment speaker=" + summary.value("speaker_id", "?")
           + " start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "text_region") {
    return "text_region type=" + summary.value("observation_type", "?")
           + " start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "text_observation") {
    return "text_obs raw=\"" + summary.value("raw_text", "?") + "\""
           + " norm=\"" + summary.value("normalized_text", "?") + "\"";
  }
  if (kind == "numeric_value") {
    return "numeric_value value=" + summary.value("numeric_value", "?")
           + " unit=" + summary.value("unit", "?");
  }
  if (kind == "evidence_crop") {
    return "crop path=" + summary.value("crop_file_path", "?")
           + " size=" + std::to_string(summary.value("crop_size_bytes", 0));
  }
  if (kind == "color_observation") {
    return "color dominant=" + summary.value("dominant_bucket", "?")
           + " target=" + summary.value("target_type", "?")
           + ":" + summary.value("target_id", "?");
  }
  if (kind == "frame") {
    return "frame pts_us=" + std::to_string(summary.value("pts_us", 0));
  }
  if (kind == "shot") {
    return "shot start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "scene") {
    return "scene start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "entity") {
    return "entity type=" + summary.value("entity_type", "?")
           + " label=" + summary.value("label", "?");
  }
  if (kind == "entity_track") {
    return "track entity=" + summary.value("entity_id", "?");
  }
  if (kind == "region") {
    return "region frame=" + summary.value("frame_id", "?");
  }
  return kind;
}

void print_traversal(const std::filesystem::path& package_path,
                     const svp::query::TraversalOptions& options,
                     bool json_output) {
  auto result = svp::query::traverse_relationships(package_path, options);

  if (json_output) {
    std::cout << svp::query::traversal_result_to_json(result).dump(2) << "\n";
    return;
  }

  if (!result.error_message.empty()) {
    std::cout << "Traversal error: " << result.error_message << "\n";
    return;
  }

  std::cout << "Traversal from " << result.start_id
            << " (depth " << result.requested_depth << ")\n";
  std::cout << "  nodes visited: " << result.visited_node_count << "\n";
  std::cout << "  edges found: " << result.edge_count << "\n";
  if (result.limit_applied) {
    std::cout << "  (limit applied)\n";
  }
  if (!result.missing_object_ids.empty()) {
    std::cout << "  unresolved IDs: " << result.missing_object_ids.size() << "\n";
    for (const auto& id : result.missing_object_ids) {
      std::cout << "    - " << id << "\n";
    }
  }
  std::cout << "\n";

  std::cout << "Nodes:\n";
  for (const auto& node : result.nodes) {
    std::cout << "  [d" << node.depth << "] " << node.object_id;
    if (node.resolved) {
      const auto desc = compact_node_summary(node.summary);
      if (!desc.empty()) {
        std::cout << "  " << desc;
      }
    } else {
      std::cout << "  (unresolved)";
    }
    std::cout << "\n";
  }

  std::cout << "\nEdges:\n";
  for (const auto& edge : result.edges) {
    std::cout << "  [d" << edge.depth << "] "
              << edge.direction << " "
              << edge.source_id << " -> " << edge.target_id
              << "  [" << edge.relationship_class << "] "
              << edge.relationship_type;
    if (!edge.relationship_id.empty()) {
      std::cout << "  id=" << edge.relationship_id;
    }
    std::cout << "\n";
  }
}

}  // namespace query_cmd

int main(int argc, char** argv) {
  CLI::App app{"SVP package inspector"};
  app.set_version_flag("--version",
                       svp::core::tool_version_label("svp-inspector"));
  app.require_subcommand(0, 1);

  std::string package_path;
  auto* inspect = app.add_subcommand("inspect", "Print a concise SVP package summary");
  inspect->add_option("package", package_path, "Path to a .svp package")->required();

  std::string dump_package_path;
  std::string dump_section = "manifest";
  auto* dump = app.add_subcommand("dump", "Print manifest or index manifest JSON");
  dump->add_option("package", dump_package_path, "Path to a .svp package")->required();
  dump->add_option("--section", dump_section,
                   "Section to dump: manifest, index_manifest, or all")
      ->check(CLI::IsMember({"manifest", "index_manifest", "all"}));

  std::string query_package_path;
  std::string query_mode = "layers";
  std::string query_text;
  std::string query_color_bucket;
  double query_min_coverage = -1.0;
  std::size_t query_limit = 100;
  bool query_json = false;
  auto* query = app.add_subcommand("query", "Query SVP package semantic layers");
  query->add_option("package", query_package_path, "Path to a .svp package")->required();
  query->add_option("--mode", query_mode,
                    "Query mode: layers, transcript, words, speakers, ocr, colors, validation, relationships, traverse")
      ->check(CLI::IsMember({"layers", "transcript", "words", "speakers",
                              "ocr", "colors", "validation",
                              "relationships", "traverse"}));
  query->add_option("--text", query_text, "Search text for words or OCR mode");
  query->add_option("--bucket", query_color_bucket, "Filter color observations by dominant bucket");
  query->add_option("--min-coverage", query_min_coverage,
                    "Minimum dominant bucket coverage (0.0-1.0)");
  query->add_option("--limit", query_limit, "Maximum results to return");
  query->add_flag("--json", query_json, "Emit JSON output for agent consumption");

  std::string query_from;
  int query_depth = 2;
  std::string query_direction = "both";
  std::string query_class = "all";
  std::string query_rel_type;
  query->add_option("--from", query_from, "Starting object ID for traverse mode");
  query->add_option("--depth", query_depth, "Max graph depth for traverse mode");
  query->add_option("--direction", query_direction,
                    "Traversal direction: outgoing, incoming, both")
      ->check(CLI::IsMember({"outgoing", "incoming", "both"}));
  query->add_option("--class", query_class,
                    "Relationship class filter: support, semantic, unknown, all")
      ->check(CLI::IsMember({"support", "semantic", "unknown", "all"}));
  query->add_option("--type", query_rel_type, "Filter by exact relationship type");

  CLI11_PARSE(app, argc, argv);

  if (*inspect) {
    const auto summary = svp::package::read_package_summary(package_path);
    print_summary(summary);
    return summary.layout_readable ? 0 : 1;
  }

  if (*dump) {
    return dump_sections(dump_package_path, dump_section);
  }

  if (*query) {
    if (query_mode == "layers") {
      query_cmd::print_layers(query_package_path, query_json);
    } else if (query_mode == "transcript") {
      query_cmd::print_transcript(query_package_path, query_json);
    } else if (query_mode == "words") {
      query_cmd::print_find_words(query_package_path, query_text, query_limit, query_json);
    } else if (query_mode == "speakers") {
      query_cmd::print_speakers(query_package_path, query_json);
    } else if (query_mode == "ocr") {
      std::optional<std::string> text_filter;
      if (!query_text.empty()) {
        text_filter = query_text;
      }
      query_cmd::print_ocr(query_package_path, text_filter, query_limit, query_json);
    } else if (query_mode == "colors") {
      std::optional<std::string> dominant_filter;
      if (!query_color_bucket.empty()) {
        dominant_filter = query_color_bucket;
      }
      std::optional<double> min_coverage;
      if (query_min_coverage >= 0.0) {
        min_coverage = query_min_coverage;
      }
      query_cmd::print_colors(query_package_path, dominant_filter, min_coverage,
                               query_limit, query_json);
    } else if (query_mode == "validation") {
      query_cmd::print_validation(query_package_path, query_json);
    } else if (query_mode == "relationships") {
      std::optional<std::string> class_filter;
      if (query_class != "all" && !query_class.empty()) {
        class_filter = query_class;
      }
      query_cmd::print_relationships(query_package_path, class_filter,
                                      query_limit, query_json);
    } else if (query_mode == "traverse") {
      svp::query::TraversalOptions opts;
      opts.start_id = query_from;
      opts.max_depth = query_depth;
      if (query_direction == "outgoing") {
        opts.direction = svp::query::TraversalDirection::Outgoing;
      } else if (query_direction == "incoming") {
        opts.direction = svp::query::TraversalDirection::Incoming;
      } else {
        opts.direction = svp::query::TraversalDirection::Both;
      }
      opts.class_filter = query_class;
      if (!query_rel_type.empty()) {
        opts.type_filter = query_rel_type;
      }
      opts.limit = query_limit;
      query_cmd::print_traversal(query_package_path, opts, query_json);
    }
    return 0;
  }

  return 0;
}
