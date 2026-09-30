#include "svp/validation/package_equivalence.hpp"

#include "equivalence/block_compare.hpp"
#include "equivalence/canonical_json_compare.hpp"
#include "equivalence/entry_layers.hpp"
#include "equivalence/equivalence_ledger.hpp"
#include "equivalence/equivalence_profile.hpp"
#include "equivalence/index_compare.hpp"
#include "equivalence/json_field_rules.hpp"
#include "equivalence/package_archive.hpp"

#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"

#include <optional>
#include <set>

namespace svp::validation {
namespace {

using namespace svp::validation::equivalence;

constexpr std::string_view kManifestEntry = "manifest.json";

enum class PackageKind {
  svp,
  svpi,
};

EquivalenceFinding eligibility_finding(std::string entry, std::string detail) {
  return EquivalenceFinding{
      .outcome = EquivalenceOutcome::not_equivalent,
      .entry = std::move(entry),
      .layer = "input",
      .rule = "comparison_eligibility",
      .detail = std::move(detail),
  };
}

// Only plain .svp and .svpi ZIP packages are accepted; both inputs must be
// the same kind (Section 5.16.1 eligibility).
std::optional<PackageKind> package_kind(const std::filesystem::path& path,
                                        EquivalenceLedger& ledger) {
  const auto probe = svp::package::probe_package(path);
  if (!probe.exists || !probe.is_regular_file) {
    ledger.add(eligibility_finding(path.string(), "input is not a readable regular file"));
    return std::nullopt;
  }
  if (probe.iso_bmff.signature_present) {
    ledger.add(eligibility_finding(
        path.string(),
        "Embedded SVPI transport containers are not supported for equivalence; "
        "compare the extracted .svpi sidecars instead"));
    return std::nullopt;
  }
  if (probe.has_svp_extension) {
    return PackageKind::svp;
  }
  if (probe.has_svpi_extension) {
    return PackageKind::svpi;
  }
  ledger.add(eligibility_finding(path.string(), "input must use the .svp or .svpi extension"));
  return std::nullopt;
}

std::optional<std::set<std::string>> read_entries(const std::filesystem::path& path,
                                                  EquivalenceLedger& ledger) {
  const auto layout = svp::package::read_package_layout(path);
  if (!layout.has_value()) {
    ledger.add(eligibility_finding(path.string(), "package ZIP is unreadable: " +
                                                      layout.error_message()));
    return std::nullopt;
  }
  if (!layout.value().invalid_entry_paths.empty()) {
    ledger.add(eligibility_finding(
        path.string(), "package contains a non-normalized entry path: " +
                           layout.value().invalid_entry_paths.front()));
    return std::nullopt;
  }
  return layout.value().entries;
}

std::optional<RasterSize> manifest_raster(const PackageArchive& archive,
                                          const std::set<std::string>& entries) {
  if (!entries.contains(std::string{kManifestEntry})) {
    return std::nullopt;
  }
  try {
    const auto manifest = nlohmann::json::parse(archive.read_entry(std::string{kManifestEntry}));
    const auto& raster = manifest.at("canonical_analysis_raster");
    const auto width = raster.at("width").get<double>();
    const auto height = raster.at("height").get<double>();
    if (width > 0.0 && height > 0.0) {
      return RasterSize{.width = width, .height = height};
    }
  } catch (const std::exception&) {
    // Without a raster, pixel-denominated rules fall back to exact.
  }
  return std::nullopt;
}

void compare_entry_set(const std::set<std::string>& left,
                       const std::set<std::string>& right,
                       EquivalenceLedger& ledger) {
  const auto report_missing = [&](const std::set<std::string>& present,
                                  const std::set<std::string>& other,
                                  std::string_view side) {
    for (const auto& entry : present) {
      if (!other.contains(entry)) {
        ledger.add(EquivalenceFinding{
            .outcome = EquivalenceOutcome::not_equivalent,
            .entry = entry,
            .layer = "package_layout",
            .rule = "entry_set",
            .detail = "entry missing from " + std::string{side} + " package",
        });
      }
    }
  };
  report_missing(left, right, "right");
  report_missing(right, left, "left");
}

void add_exact_bytes_finding(const std::string& entry,
                             std::string_view layer,
                             std::uint64_t offset,
                             std::string detail,
                             EquivalenceLedger& ledger) {
  ledger.add(EquivalenceFinding{
      .outcome = EquivalenceOutcome::not_equivalent,
      .entry = entry,
      .layer = std::string{layer},
      .rule = "exact_blake3",
      .location = "byte " + std::to_string(offset),
      .detail = std::move(detail),
  });
}

void compare_differing_entry(const std::string& entry,
                             std::uint64_t first_difference,
                             const std::filesystem::path& left_path,
                             const std::filesystem::path& right_path,
                             const PackageArchive& left,
                             const PackageArchive& right,
                             const JsonCompareSettings& settings,
                             EquivalenceLedger& ledger) {
  const auto layer = classify_entry(entry);
  switch (layer.layer) {
    case EntryLayer::directory:
      return;
    case EntryLayer::sqlite_index:
      compare_index_entry(entry, left_path, right_path, settings.normalize_build_metadata,
                          ledger);
      return;
    case EntryLayer::block_stream:
      compare_block_stream_entry(entry, *layer.block_type, left.read_entry(entry),
                                 right.read_entry(entry), ledger);
      return;
    case EntryLayer::canonical_json:
    case EntryLayer::canonical_jsonl: {
      const auto kind = layer.layer == EntryLayer::canonical_json ? JsonEntryKind::json
                                                                  : JsonEntryKind::jsonl;
      if (!compare_json_entry(entry, kind, left.read_entry(entry), right.read_entry(entry),
                              settings, ledger)) {
        add_exact_bytes_finding(entry, layer.name, first_difference,
                                "entry is not parseable JSON; compared as exact bytes",
                                ledger);
      }
      return;
    }
    case EntryLayer::exact_bytes:
      add_exact_bytes_finding(entry, layer.name, first_difference,
                              "entry bytes differ (exact-governed layer)", ledger);
      return;
  }
}

void compare_package_contents(const std::filesystem::path& left_path,
                              const std::filesystem::path& right_path,
                              const EquivalenceOptions& options,
                              EquivalenceReport& report,
                              EquivalenceLedger& ledger) {
  const auto left_entries = read_entries(left_path, ledger);
  const auto right_entries = read_entries(right_path, ledger);
  if (!left_entries || !right_entries) {
    return;
  }
  compare_entry_set(*left_entries, *right_entries, ledger);

  const PackageArchive left{left_path};
  const PackageArchive right{right_path};
  const JsonCompareSettings settings{
      .normalize_build_metadata = options.normalize_build_metadata,
      .raster = manifest_raster(left, *left_entries),
  };

  for (const auto& entry : *left_entries) {
    if (!right_entries->contains(entry)) {
      continue;
    }
    ++report.entries_compared;
    ledger.mark_compared(entry);
    try {
      const auto difference = PackageArchive::first_difference(left, right, entry);
      if (difference.has_value()) {
        compare_differing_entry(entry, *difference, left_path, right_path, left, right,
                                settings, ledger);
      }
    } catch (const std::exception& error) {
      ledger.add(EquivalenceFinding{
          .outcome = EquivalenceOutcome::not_equivalent,
          .entry = entry,
          .layer = std::string{classify_entry(entry).name},
          .rule = "entry_readable",
          .detail = error.what(),
      });
    }
  }
}

}  // namespace

EquivalenceReport compare_packages(const std::filesystem::path& left,
                                   const std::filesystem::path& right,
                                   const EquivalenceOptions& options) {
  EquivalenceReport report;
  report.profile = std::string{DefaultEquivalenceProfileV1::kName};
  report.normalize_build_metadata = options.normalize_build_metadata;
  report.left.path = left.string();
  report.right.path = right.string();
  for (const auto note : exact_fallback_notes()) {
    report.exact_fallbacks.emplace_back(note);
  }

  EquivalenceLedger ledger;
  const auto left_kind = package_kind(left, ledger);
  const auto right_kind = package_kind(right, ledger);
  if (left_kind && right_kind && *left_kind != *right_kind) {
    ledger.add(eligibility_finding(
        "", "inputs are different package kinds (.svp vs .svpi) and are not "
            "eligible for equivalence comparison"));
  }

  if (left_kind && right_kind && *left_kind == *right_kind) {
    try {
      const auto left_identity = file_identity(left);
      const auto right_identity = file_identity(right);
      report.left.blake3 = left_identity.blake3;
      report.left.size_bytes = left_identity.size_bytes;
      report.right.blake3 = right_identity.blake3;
      report.right.size_bytes = right_identity.size_bytes;
      if (left_identity.blake3 == right_identity.blake3 &&
          left_identity.size_bytes == right_identity.size_bytes) {
        report.classification = EquivalenceClass::byte_identical;
        return report;
      }
      compare_package_contents(left, right, options, report, ledger);
    } catch (const std::exception& error) {
      ledger.add(eligibility_finding("", std::string{"comparison failed: "} + error.what()));
    }
  }

  ledger.resolve_deferred_digests();
  report.classification = ledger.classification();
  report.findings = ledger.findings();
  return report;
}

int exit_code(const EquivalenceReport& report) noexcept {
  return report.classification == EquivalenceClass::not_equivalent ? 1 : 0;
}

}  // namespace svp::validation
