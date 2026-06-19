#include "svp/validation/validator.hpp"

#include "svp/core/version.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/validation/code_registry.hpp"

#include <array>
#include <exception>

namespace svp::validation {
namespace {

constexpr std::array<std::string_view, 9> kRequiredTopLevelSections{
    "media", "transcript", "timeline",      "entities", "spatial",
    "relationships", "embeddings", "index", "provenance"};

ValidationReport make_report(const std::filesystem::path& package_path) {
  ValidationReport report;
  report.validator = ValidatorIdentity{
      .name = "svp-validator",
      .version = std::string{svp::core::kToolVersion},
  };
  report.package_path = package_path.string();
  return report;
}

std::string package_path_for_report(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  return path.string();
}

void mark_unreadable(ValidationReport& report) noexcept {
  report.status = ValidationStatus::unreadable;
  report.core_status = ValidationStatus::unreadable;
}

bool add_input_findings(ValidationReport& report,
                        const ValidationCodeRegistry& registry,
                        const svp::package::PackageProbe& probe) {
  if (!probe.exists) {
    add_finding(report, make_finding(registry, kTempCodeInputMissing,
                                     package_path_for_report(probe.path),
                                     "Input file does not exist."));
    mark_unreadable(report);
    return false;
  }

  if (!probe.is_regular_file) {
    add_finding(report, make_finding(registry, kTempCodeInputNotRegularFile,
                                     package_path_for_report(probe.path),
                                     "Input path is not a regular file."));
    mark_unreadable(report);
    return false;
  }

  if (!probe.has_svp_extension) {
    add_finding(report, make_finding(registry, kTempCodeWrongExtension,
                                     package_path_for_report(probe.path),
                                     "Input file must use the .svp extension."));
    mark_unreadable(report);
    return false;
  }

  return true;
}

void add_layout_findings(ValidationReport& report,
                         const ValidationCodeRegistry& registry,
                         const svp::package::PackageLayout& layout) {
  for (const auto& invalid_path : layout.invalid_entry_paths) {
    add_finding(report, make_finding(registry, kCodePathTraversal, "/" + invalid_path,
                                     "ZIP entry path is not normalized inside the package."));
  }

  if (!layout.has_entry("manifest.json")) {
    add_finding(report, make_finding(registry, kCodeMissingManifest, "/manifest.json",
                                     "manifest.json is absent."));
  }

  if (!layout.has_entry("mimetype")) {
    add_finding(report, make_finding(registry, kCodeMissingSection, "/mimetype",
                                     "Required top-level package entry is absent."));
  }

  for (const auto section : kRequiredTopLevelSections) {
    if (!layout.has_top_level_section(std::string{section})) {
      add_finding(report, make_finding(registry, kCodeMissingSection,
                                       "/" + std::string{section} + "/",
                                       "Required top-level package section is absent."));
    }
  }

  for (const auto& root : layout.root_entries) {
    if (root == "mimetype" || root == "manifest.json" || root == "labels") {
      continue;
    }

    bool known_section = false;
    for (const auto section : kRequiredTopLevelSections) {
      if (root == section) {
        known_section = true;
        break;
      }
    }

    if (!known_section) {
      add_finding(report, make_finding(registry, kCodeUnknownRootSection,
                                       "/" + root,
                                       "Unknown root-level package section is not allowed."));
    }
  }
}

}  // namespace

ValidationReport validate_package(const std::filesystem::path& package_path,
                                  const ValidatorOptions& options) {
  auto report = make_report(package_path);

  ValidationCodeRegistry registry;
  try {
    registry = load_validation_code_registry(options.validation_codes_path);
  } catch (const std::exception& error) {
    add_finding(report, make_runtime_finding(kTempCodeRegistryUnreadable,
                                             options.validation_codes_path.string(),
                                             error.what()));
    mark_unreadable(report);
    return report;
  }

  const auto probe = svp::package::probe_package(package_path);
  if (!add_input_findings(report, registry, probe)) {
    return report;
  }

  auto layout_result = svp::package::read_package_layout(probe.path);
  if (!layout_result.has_value()) {
    add_finding(report, make_finding(registry, kTempCodeZipUnreadable,
                                     package_path_for_report(probe.path),
                                     layout_result.error_message()));
    mark_unreadable(report);
    return report;
  }

  add_layout_findings(report, registry, layout_result.value());
  recompute_status(report);
  return report;
}

}  // namespace svp::validation
