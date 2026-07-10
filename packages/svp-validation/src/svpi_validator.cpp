#include "svp/validation/svpi_validator.hpp"

#include "svp/core/version.hpp"
#include "svp/package/package_contract.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_probe.hpp"
#include "svp/package/media_binding.hpp"
#include "svp/package/svpi_media_policy.hpp"
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
                        const svp::package::PackageProbe& probe,
                        bool allow_embedded_mp4) {
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

  if (!probe.has_svpi_extension && !(allow_embedded_mp4 && probe.has_mp4_extension)) {
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

  const auto primary_binding_id_it = binding.find("primary_binding_id");
  if (primary_binding_id_it == binding.end() || !primary_binding_id_it->is_string()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "media_binding.json must contain primary_binding_id."));
    return;
  }

  const auto bindings_it = binding.find("bindings");
  if (bindings_it == binding.end() || !bindings_it->is_array()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "media_binding.json must contain a bindings array."));
    return;
  }

  if (bindings_it->size() != 1) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "SVPI v0.1 requires exactly one binding in the bindings array."));
    return;
  }

  const auto& primary = bindings_it->at(0);
  if (!primary.is_object()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "binding must be a JSON object."));
    return;
  }

  const auto role_it = primary.find("media_role");
  if (role_it == primary.end() || !role_it->is_string() ||
      role_it->get<std::string>() != "primary_source") {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "binding media_role must be primary_source."));
    return;
  }

  const std::vector<std::string_view> required_string_fields = {
      "binding_id", "media_role", "media_id", "container_format",
      "verification_state",
  };
  for (const auto& field : required_string_fields) {
    if (!primary.contains(field) || !primary[field].is_string()) {
      add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                       "/media_binding.json",
                                       std::string("binding must contain ") + std::string(field) + "."));
    }
  }

  const std::vector<std::string_view> required_int_fields = {
      "duration_us", "size_bytes",
  };
  for (const auto& field : required_int_fields) {
    if (!primary.contains(field)) {
      add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                       "/media_binding.json",
                                       std::string("binding must contain ") + std::string(field) + "."));
    }
  }

  if (!primary.contains("streams") || !primary["streams"].is_array()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "binding must contain streams array."));
  }

  if (!primary.contains("location_hints") || !primary["location_hints"].is_object()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "binding must contain location_hints object."));
  }

  const auto contract_it = primary.find("binding_contract");
  if (contract_it == primary.end() || !contract_it->is_string()) {
    add_finding(report, make_finding(registry, kCodeSvpiWrongBindingContract,
                                     "/media_binding.json",
                                     "binding must contain binding_contract."));
  } else if (contract_it->get<std::string>() != std::string{svp::package::kSvpiBindingContract}) {
    add_finding(report, make_finding(registry, kCodeSvpiWrongBindingContract,
                                     "/media_binding.json",
                                     "binding_contract must be svpi.media_identity.v0.1."));
  }

  const auto identity = primary.find("identity");
  if (identity == primary.end() || !identity->is_object()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "binding must contain identity."));
    return;
  }

  const auto full_file_blake3 = identity->find("full_file_blake3");
  if (full_file_blake3 == identity->end() || !full_file_blake3->is_object()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "identity must contain full_file_blake3 object."));
  } else {
    const auto blake3_state = full_file_blake3->find("state");
    if (blake3_state == full_file_blake3->end() || !blake3_state->is_string()) {
      add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                       "/media_binding.json",
                                       "identity.full_file_blake3 must contain state."));
    } else {
      const auto state_val = blake3_state->get<std::string>();
      if (state_val != "present" && state_val != "pending" && state_val != "unavailable") {
        add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                         "/media_binding.json",
                                         "identity.full_file_blake3.state must be present, pending, or unavailable."));
      }
      if (state_val == "present") {
        const auto blake3_value = full_file_blake3->find("value");
        if (blake3_value == full_file_blake3->end() || !blake3_value->is_string() ||
            blake3_value->get<std::string>().empty()) {
          add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                           "/media_binding.json",
                                           "identity.full_file_blake3.value must be non-empty when state is present."));
        }
      }
      if (state_val == "unavailable") {
        const auto reason = full_file_blake3->find("reason");
        if (reason == full_file_blake3->end() || !reason->is_string() ||
            reason->get<std::string>().empty()) {
          add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                           "/media_binding.json",
                                           "identity.full_file_blake3.reason must be recorded when state is unavailable."));
        }
      }
    }
  }

  const auto chunk_hashes = identity->find("chunk_hashes");
  if (chunk_hashes == identity->end() || !chunk_hashes->is_object()) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                     "/media_binding.json",
                                     "identity must contain chunk_hashes object."));
  } else {
    const auto chunk_algo = chunk_hashes->find("algorithm");
    if (chunk_algo == chunk_hashes->end() || !chunk_algo->is_string()) {
      add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                       "/media_binding.json",
                                       "identity.chunk_hashes must contain algorithm."));
    }
    const auto chunk_size = chunk_hashes->find("chunk_size_bytes");
    if (chunk_size == chunk_hashes->end() || !chunk_size->is_number_unsigned()) {
      add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                       "/media_binding.json",
                                       "identity.chunk_hashes must contain chunk_size_bytes."));
    }
    const auto chunk_count = chunk_hashes->find("chunk_count");
    if (chunk_count == chunk_hashes->end() || !chunk_count->is_number_unsigned()) {
      add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                       "/media_binding.json",
                                       "identity.chunk_hashes must contain chunk_count."));
    }
  }

  const auto vs_it = primary.find("verification_state");
  if (vs_it != primary.end() && vs_it->is_string()) {
    const auto vs = vs_it->get<std::string>();
    if (vs != "verified" && vs != "pending" && vs != "mismatch" && vs != "unavailable") {
      add_finding(report, make_finding(registry, kCodeSvpiMissingMediaBinding,
                                       "/media_binding.json",
                                       "verification_state must be verified, pending, mismatch, or unavailable."));
    }
  }
}

void add_svpi_forbidden_media_findings(ValidationReport& report,
                                       const ValidationCodeRegistry& registry,
                                       const svp::package::PackageLayout& layout) {
  for (const auto& entry : layout.entries) {
    const auto reason = svp::package::classify_svpi_entry(entry);
    if (reason == svp::package::SvpiForbiddenMediaReason::primary_media) {
      add_finding(report, make_finding(registry, kCodeSvpiForbiddenPrimaryMedia,
                                       "/" + entry,
                                       "SVPI must not contain media/original/ entries."));
    } else if (reason == svp::package::SvpiForbiddenMediaReason::replayable_derivative) {
      add_finding(report, make_finding(registry, kCodeSvpiForbiddenReplayableMediaDerivative,
                                       "/" + entry,
                                       "SVPI must not contain replayable source-derived audio, video, or muxed media derivatives."));
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
                             const std::filesystem::path& path,
                             const svp::package::PackageLayout& layout) {
  if (!layout.has_entry("index/index.sqlite")) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingIndex,
                                     "/index/index.sqlite",
                                     "index/index.sqlite is required for SVPI."));
  } else {
    const auto sqlite_data = svp::package::read_package_entry(path, "index/index.sqlite");
    if (!sqlite_data.has_value()) {
      add_finding(report, make_finding(registry, kCodeSvpiMissingIndex,
                                       "/index/index.sqlite",
                                       "index/index.sqlite is unreadable."));
    } else {
      const auto& data = sqlite_data.value();
      if (data.size() < 16 || data.substr(0, 15) != "SQLite format 3") {
        add_finding(report, make_finding(registry, kCodeSvpiMissingIndex,
                                         "/index/index.sqlite",
                                         "index/index.sqlite is not a valid SQLite database."));
      }
    }
  }

  if (!layout.has_entry("index/index_manifest.json")) {
    add_finding(report, make_finding(registry, kCodeSvpiMissingIndex,
                                     "/index/index_manifest.json",
                                     "index/index_manifest.json is required for SVPI."));
  } else {
    const auto manifest_data = svp::package::read_package_entry(path, "index/index_manifest.json");
    if (!manifest_data.has_value()) {
      add_finding(report, make_finding(registry, kCodeSvpiMissingIndex,
                                       "/index/index_manifest.json",
                                       "index/index_manifest.json is unreadable."));
    } else {
      const auto manifest = nlohmann::json::parse(manifest_data.value(), nullptr, false);
      if (manifest.is_discarded() || !manifest.is_object()) {
        add_finding(report, make_finding(registry, kCodeSvpiMissingIndex,
                                         "/index/index_manifest.json",
                                         "index/index_manifest.json is not valid JSON."));
      }
    }
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
  if (!add_input_findings(report, registry, probe, options.allow_embedded_mp4)) {
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
  add_svpi_index_findings(report, registry, probe.path, layout);

  recompute_status(report);
  return report;
}

}  // namespace svp::validation
