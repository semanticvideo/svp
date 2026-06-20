#include "svp/core/version.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_summary.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
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

  CLI11_PARSE(app, argc, argv);

  if (*inspect) {
    const auto summary = svp::package::read_package_summary(package_path);
    print_summary(summary);
    return summary.layout_readable ? 0 : 1;
  }

  if (*dump) {
    return dump_sections(dump_package_path, dump_section);
  }

  return 0;
}
