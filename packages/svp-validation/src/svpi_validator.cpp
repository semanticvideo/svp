#include "svp/validation/svpi_validator.hpp"

#include "svp/core/version.hpp"
#include "svp/package/package_contract.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/package/media_binding.hpp"
#include "svp/validation/code_registry.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <string_view>

namespace svp::validation {
namespace {

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

std::filesystem::path registry_root_for(const SvpiValidatorOptions& options) {
  if (!options.registry_root_path.empty()) {
    return options.registry_root_path;
  }
  return options.validation_codes_path.parent_path();
}

std::filesystem::path schema_root_for(const SvpiValidatorOptions& options,
                                      const std::filesystem::path& registry_root) {
  if (!options.schema_root_path.empty()) {
    return options.schema_root_path;
  }
  return registry_root.parent_path() / "schemas";
}

bool add_input_findings(ValidationReport& report,
                        const ValidationCodeRegistry& registry,
                        const svp::package::PackageProbe& probe) {
  if (!probe.exists) {
    add_finding(report, make_runtime_finding(kTempCodeInputMissing,
                                             package_path_for_report(probe.path),
                                             "Input file does not exist."));
    mark_unreadable(report);
    return false;
  }

  if (!probe.is_regular_file) {
    add_finding(report, make_runtime_finding(kTempCodeInputNotRegularFile,
                                             package_path_for_report(probe.path),
                                             "Input path is not a regular file."));
    mark_unreadable(report);
    return false;
  }

  if (!probe.has_svpi_extension) {
    add_finding(report, make_runtime_finding(kTempCodeWrongExtension,
                                             package_path_for_report(probe.path),
                                             "Input file must use the .svpi extension."));
    mark_unreadable(report);
    return false;
  }

  return true;
}

void add_svpi_mimetype_findings(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                const std::filesystem::path& path,
                                const svp::package::PackageLayout& layout) {
  if (!layout.has_entry("mimetype")) {
    add_finding(report, make_finding(registry, kCodeMissingSection,
                                     "/mimetype",
                                     "Required mimetype entry is absent."));
    return;
  }

  const auto read_result = svp::package::read_package_entry(path, "mimetype");
  if (!read_result.has_value()) {
    add_finding(report, make_finding(registry, kCodeSvpiWrongMimetype,
                                     "/mimetype",
                                     "mimetype entry is unreadable: " + read_result.error_message()));
    return;
  }

  const std::string& content = read_result.value();
  if (content != std::string{svp::package::kSvpiMimetype}) {
    add_finding(report, make_finding(registry, kCodeSvpiWrongMimetype,
                                     "/mimetype",
                                     "mimetype must be application/vnd.svp.interlace+zip"));
  }
}

void add_svpi_manifest_findings(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                const std::filesystem::path& path,
                                const svp::package::PackageLayout& layout) {
  if (layout.has_entry("svpi_manifest.json")) {
    add_finding(report, make_finding(registry, kCodeSvpiLegacyManifestName,
                                     "/svpi_manifest.json",
                                     "SVPI must use manifest.json, not svpi_manifest.json."));
  }

  if (!layout.has_entry("manifest.json")) {
    add_finding(report, make_finding(registry, kCodeMissingManifest,
                                     "/manifest.json",
                                     "manifest.json is absent."));
    return;
  }

  const auto read_result = svp::package::read_package_entry(path, "manifest.json");
  if (!read_result.has_value()) {
    add_finding(report, make_finding(registry, kCodeMissingManifest,
                                     "/manifest.json",
                                     "manifest.json is unreadable."));
    return;
  }

  const auto manifest = nlohmann::json::parse(read_result.value(), nullptr, false);
  if (manifest.is_discarded() || !manifest.is_object()) {
    add_finding(report, make_finding(registry, kCodeMissingManifest,
                                     "/manifest.json",
                                     "manifest.json is not valid JSON."));
    return;
  }

  const auto format_it = manifest.find("format");
  if (format_it == manifest.end() || !format_it->is_string()) {
    add_finding(report, make_finding(registry, kCodeSvpiWrongManifestFormat,
                                     "/manifest.json",
                                     "manifest.json must contain a format field."));
  } else if (format_it->get<std::string>() != "svpi") {
    add_finding(report, make_finding(registry, kCodeSvpiWrongManifestFormat,
                                     "/manifest.json",
                                     "manifest.json format must be \"svpi\"."));
  }
}

void add_svpi_media_binding_findings(ValidationReport& report,
                                     const ValidationCodeRegistry& registry,
                                     const std::filesystem::path& path,
                                     const svp::package::PackageLayout& layout) {
  if (!layout.has_entry("media_binding.json")) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "media_binding.json is required for SVPI."));
    return;
  }

  const auto read_result = svp::package::read_package_entry(path, "media_binding.json");
  if (!read_result.has_value()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "media_binding.json is unreadable."));
    return;
  }

  const auto binding = nlohmann::json::parse(read_result.value(), nullptr, false);
  if (binding.is_discarded() || !binding.is_object()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "media_binding.json is not valid JSON."));
    return;
  }

  const auto primary = binding.find("primary_source");
  if (primary == binding.end() || !primary->is_object()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "media_binding.json must contain primary_source."));
    return;
  }

  const auto contract_it = primary->find("binding_contract");
  if (contract_it == primary->end() || !contract_it->is_string()) {
    add_finding(report, make_finding(registry, kCodeSvpiWrongBindingContract,
                                     "/media_binding.json",
                                     "primary_source must contain binding_contract."));
  } else if (contract_it->get<std::string>() != std::string{svp::package::kSvpiBindingContract}) {
    add_finding(report, make_finding(registry, kCodeSvpiWrongBindingContract,
                                     "/media_binding.json",
                                     "binding_contract must be svpi.media_identity.v0.1."));
  }

  const auto identity = primary->find("identity");
  if (identity == primary->end() || !identity->is_object()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "primary_source must contain identity."));
    return;
  }

  if (!identity->contains("media_id") || !(*identity)["media_id"].is_string()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "identity must contain media_id."));
  }

  if (!identity->contains("size_bytes")) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "identity must contain size_bytes."));
  }
}

void add_svpi_forbidden_media_findings(ValidationReport& report,
                                       const ValidationCodeRegistry& registry,
                                       const svp::package::PackageLayout& layout) {
  for (const auto& entry : layout.entries) {
    if (entry.rfind("media/original/", 0) == 0) {
      add_finding(report, make_finding(registry, kCodeSvpiForbiddenPrimaryMedia,
                                       "/" + entry,
                                       "SVPI must not contain media/original/ entries."));
    }
  }
}

void add_svpi_provenance_findings(ValidationReport& report,
                                  const ValidationCodeRegistry& registry,
                                  const svp::package::PackageLayout& layout) {
  if (!layout.has_entry("provenance/processors.jsonl")) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingProvenance,
                                     "/provenance/processors.jsonl",
                                     "provenance/processors.jsonl is required for SVPI."));
  }

  if (!layout.has_entry("provenance/interlace_events.jsonl")) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingProvenance,
                                     "/provenance/interlace_events.jsonl",
                                     "provenance/interlace_events.jsonl is required for SVPI."));
  }
}

void add_svpi_index_findings(ValidationReport& report,
                             const ValidationCodeRegistry& registry,
                             const svp::package::PackageLayout& layout) {
  if (!layout.has_entry("index/index.sqlite")) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingIndex,
                                     "/index/index.sqlite",
                                     "index/index.sqlite is required for SVPI."));
  }

  if (!layout.has_entry("index/index_manifest.json")) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingIndex,
                                     "/index/index_manifest.json",
                                     "index/index_manifest.json is required for SVPI."));
  }
}

void add_layout_path_findings(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const svp::package::PackageLayout& layout) {
  for (const auto& invalid_path : layout.invalid_entry_paths) {
    add_finding(report, make_finding(registry, kCodePathTraversal,
                                     "/" + invalid_path,
                                     "ZIP entry path is not normalized inside the package."));
  }
}

}  // namespace

ValidationReport validate_svpi_package(
    const std::filesystem::path& package_path,
    const SvpiValidatorOptions& options) {
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
    add_finding(report, make_runtime_finding(kTempCodeZipUnreadable,
                                             package_path_for_report(probe.path),
                                             layout_result.error_message()));
    mark_unreadable(report);
    return report;
  }

  const auto& layout = layout_result.value();

  add_layout_path_findings(report, registry, layout);
  add_svpi_mimetype_findings(report, registry, probe.path, layout);
  add_svpi_manifest_findings(report, registry, probe.path, layout);
  add_svpi_media_binding_findings(report, registry, probe.path, layout);
  add_svpi_forbidden_media_findings(report, registry, layout);
  add_svpi_provenance_findings(report, registry, layout);
  add_svpi_index_findings(report, registry, layout);

  recompute_status(report);
  return report;
}

}  // namespace svp::validation
