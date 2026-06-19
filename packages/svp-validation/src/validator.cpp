#include "svp/validation/validator.hpp"

#include "svp/core/version.hpp"
#include "svp/package/package_contract.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/validation/code_registry.hpp"

#include "color_record_validation.hpp"
#include "ocr_color_spec.hpp"
#include "spec_assets.hpp"
#include "text_record_validation.hpp"

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

std::filesystem::path registry_root_for(const ValidatorOptions& options) {
  if (!options.registry_root_path.empty()) {
    return options.registry_root_path;
  }

  return options.validation_codes_path.parent_path();
}

std::filesystem::path schema_root_for(const ValidatorOptions& options,
                                      const std::filesystem::path& registry_root) {
  if (!options.schema_root_path.empty()) {
    return options.schema_root_path;
  }

  return registry_root.parent_path() / "schemas";
}

std::filesystem::path asset_path_for(const SpecAsset& asset,
                                     const std::filesystem::path& registry_root,
                                     const std::filesystem::path& schema_root) {
  switch (asset.kind) {
    case SpecAssetKind::registry:
      return registry_root / std::string{asset.relative_path};
    case SpecAssetKind::schema:
      return schema_root / std::string{asset.relative_path};
  }

  return registry_root / std::string{asset.relative_path};
}

std::string_view unreadable_code_for(SpecAssetKind kind) noexcept {
  switch (kind) {
    case SpecAssetKind::registry:
      return kTempCodeRegistryUnreadable;
    case SpecAssetKind::schema:
      return kTempCodeSchemaUnreadable;
  }

  return kTempCodeRegistryUnreadable;
}

std::string_view invalid_code_for(SpecAssetKind kind) noexcept {
  switch (kind) {
    case SpecAssetKind::registry:
      return kTempCodeRegistryInvalid;
    case SpecAssetKind::schema:
      return kTempCodeSchemaInvalid;
  }

  return kTempCodeRegistryInvalid;
}

bool add_spec_asset_findings(ValidationReport& report,
                             const ValidationCodeRegistry& registry,
                             const ValidatorOptions& options) {
  bool assets_loaded = true;
  const auto registry_root = registry_root_for(options);
  const auto schema_root = schema_root_for(options, registry_root);

  for (const auto& asset : required_rc2_spec_assets()) {
    const auto asset_path = asset_path_for(asset, registry_root, schema_root);
    try {
      load_json_spec_asset(asset_path);
    } catch (const nlohmann::json::exception& error) {
      add_finding(report, make_finding(registry, invalid_code_for(asset.kind),
                                       asset_path.string(), error.what()));
      assets_loaded = false;
    } catch (const std::exception& error) {
      add_finding(report, make_finding(registry, unreadable_code_for(asset.kind),
                                       asset_path.string(), error.what()));
      assets_loaded = false;
    }
  }

  if (!assets_loaded) {
    mark_unreadable(report);
  }

  return assets_loaded;
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

std::string report_path_for(const svp::package::PackageLayoutRequirement& requirement) {
  return "/" + std::string{requirement.path} +
         (requirement.kind == svp::package::PackageLayoutRequirementKind::required_top_level_section
              ? "/"
              : "");
}

std::string_view missing_layout_code_for(
    svp::package::PackageLayoutRequirementArea area) noexcept {
  switch (area) {
    case svp::package::PackageLayoutRequirementArea::text:
      return kCodeMissingTextSection;
    case svp::package::PackageLayoutRequirementArea::colors:
      return kCodeMissingColorSection;
    case svp::package::PackageLayoutRequirementArea::core:
      return kCodeMissingSection;
  }

  return kCodeMissingSection;
}

bool has_layout_requirement(const svp::package::PackageLayout& layout,
                            const svp::package::PackageLayoutRequirement& requirement) {
  switch (requirement.kind) {
    case svp::package::PackageLayoutRequirementKind::required_entry:
      return layout.has_entry(std::string{requirement.path});
    case svp::package::PackageLayoutRequirementKind::required_top_level_section:
      return layout.has_top_level_section(std::string{requirement.path});
  }

  return false;
}

std::string missing_layout_message_for(
    const svp::package::PackageLayoutRequirement& requirement) {
  if (requirement.kind ==
      svp::package::PackageLayoutRequirementKind::required_top_level_section) {
    return "Required top-level package section is absent.";
  }

  return "Required package entry is absent.";
}

void add_layout_findings(ValidationReport& report,
                         const ValidationCodeRegistry& registry,
                         const svp::package::PackageLayout& layout) {
  for (const auto& invalid_path : layout.invalid_entry_paths) {
    add_finding(report, make_finding(registry, kCodePathTraversal, "/" + invalid_path,
                                     "ZIP entry path is not normalized inside the package."));
  }

  for (const auto& requirement : svp::package::required_package_layout()) {
    if (has_layout_requirement(layout, requirement)) {
      continue;
    }

    if (requirement.path == "manifest.json") {
      add_finding(report, make_finding(registry, kCodeMissingManifest, "/manifest.json",
                                       "manifest.json is absent."));
      continue;
    }

    add_finding(report, make_finding(registry, missing_layout_code_for(requirement.area),
                                     report_path_for(requirement),
                                     missing_layout_message_for(requirement)));
  }

  for (const auto& root : layout.root_entries) {
    if (!svp::package::is_allowed_root_entry(root)) {
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

  if (!add_spec_asset_findings(report, registry, options)) {
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

  try {
    const auto registry_root = registry_root_for(options);
    const auto schema_root = schema_root_for(options, registry_root);
    const auto ocr_color_spec = load_ocr_color_spec(registry_root, schema_root);
    add_text_record_findings(report, registry, probe.path, layout_result.value(),
                             ocr_color_spec);
    add_color_record_findings(report, registry, probe.path, layout_result.value(),
                              ocr_color_spec);
  } catch (const nlohmann::json::exception& error) {
    const auto registry_root = registry_root_for(options);
    add_finding(report, make_finding(registry, kTempCodeRegistryInvalid,
                                     registry_root.string(), error.what()));
    mark_unreadable(report);
    return report;
  } catch (const std::exception& error) {
    const auto registry_root = registry_root_for(options);
    add_finding(report, make_finding(registry, kTempCodeRegistryUnreadable,
                                     registry_root.string(), error.what()));
    mark_unreadable(report);
    return report;
  }

  recompute_status(report);
  return report;
}

}  // namespace svp::validation
