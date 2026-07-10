#include "svp/validation/embedded_svpi_transport_validator.hpp"

#include "svp/core/version.hpp"
#include "svp/package/embedded_svpi.hpp"
#include "svp/package/embedded_svpi_transport_profile.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/svpi_validator.hpp"

#include "embedded_media_binding_validation.hpp"

#include <exception>
#include <string>
#include <string_view>
#include <system_error>

namespace svp::validation {
namespace {

std::string_view issue_validation_code(svp::package::EmbeddedSvpiIssueCode code) {
  using Issue = svp::package::EmbeddedSvpiIssueCode;
  switch (code) {
    case Issue::input_unreadable:
    case Issue::invalid_box_structure:
      return kCodeIsoBmffBoxStructureInvalid;
    case Issue::unsupported_container:
      return kCodeIsoBmffUnsupportedContainer;
    case Issue::truncated_uuid_box:
      return kCodeIsoBmffUuidBoxTruncated;
    case Issue::unsupported_profile_version:
      return kCodeIsoBmffSvpiProfileUnsupported;
    case Issue::invalid_envelope_size:
    case Issue::unsupported_flags:
    case Issue::invalid_envelope_magic:
      return kCodeIsoBmffSvpiEnvelopeInvalid;
    case Issue::payload_outside_file:
      return kCodeIsoBmffSvpiPayloadBounds;
    case Issue::payload_length_mismatch:
      return kCodeIsoBmffSvpiPayloadLengthMismatch;
    case Issue::payload_hash_mismatch:
      return kCodeIsoBmffSvpiPayloadHashMismatch;
    case Issue::duplicate_embeddings:
      return kCodeIsoBmffSvpiDuplicate;
    case Issue::no_embedding:
      return kCodeIsoBmffSvpiNotFound;
    case Issue::unsafe_tail_layout:
      return kCodeIsoBmffUnsafeTailLayout;
    case Issue::zero_sized_top_level_box:
      return kCodeIsoBmffZeroSizedBox;
    case Issue::output_exists:
    case Issue::output_write_failed:
      return kCodeIsoBmffBoxStructureInvalid;
  }
  return kCodeIsoBmffBoxStructureInvalid;
}

void merge_findings(ValidationReport& destination,
                    const ValidationReport& source) {
  destination.errors.insert(destination.errors.end(),
                            source.errors.begin(), source.errors.end());
  destination.warnings.insert(destination.warnings.end(),
                              source.warnings.begin(), source.warnings.end());
  destination.infos.insert(destination.infos.end(),
                           source.infos.begin(), source.infos.end());
  destination.authenticity.insert(destination.authenticity.end(),
                                  source.authenticity.begin(),
                                  source.authenticity.end());
}

std::string transport_status(const svp::package::EmbeddedSvpiInspection& inspection) {
  if (!inspection.container.signature_present) {
    return "unsupported_container";
  }
  if (!inspection.container.structure_valid) {
    return "malformed_container";
  }
  if (!inspection.container.supported) {
    return "unsupported_container";
  }
  if (inspection.embeddings.empty()) {
    return "no_embedded_svpi";
  }
  return "invalid_embedding";
}

}  // namespace

std::string_view validation_code_for_embedded_issue(
    svp::package::EmbeddedSvpiIssueCode code) noexcept {
  return issue_validation_code(code);
}

ValidationReport validate_embedded_svpi_transport(
    const std::filesystem::path& container_path,
    const EmbeddedSvpiTransportValidatorOptions& options) {
  ValidationReport report;
  report.validator = {
      .name = "svp-validator",
      .version = std::string{svp::core::kToolVersion},
  };
  report.package_path = container_path.string();
  report.embedding_transport.present = true;
  report.embedding_transport.profile =
      std::string{svp::package::kEmbeddedSvpiTransportProfileName};
  report.embedding_transport.uuid =
      std::string{svp::package::kEmbeddedSvpiTransportUuidText};

  ValidationCodeRegistry registry;
  try {
    registry = load_validation_code_registry(options.validation_codes_path);
  } catch (const std::exception& error) {
    add_finding(report, make_runtime_finding(
        kTempCodeRegistryUnreadable, options.validation_codes_path.string(),
        error.what()));
    report.status = ValidationStatus::unreadable;
    report.core_status = ValidationStatus::unreadable;
    report.embedding_transport.status = "unreadable";
    return report;
  }

  std::error_code filesystem_error;
  const auto status = std::filesystem::status(
      container_path, filesystem_error);
  if (filesystem_error || !std::filesystem::exists(status)) {
    add_finding(report, make_runtime_finding(
        kTempCodeInputMissing, container_path.string(),
        "Input file does not exist."));
    report.status = ValidationStatus::unreadable;
    report.core_status = ValidationStatus::unreadable;
    report.embedding_transport.status = "unreadable";
    return report;
  }
  if (!std::filesystem::is_regular_file(status)) {
    add_finding(report, make_runtime_finding(
        kTempCodeInputNotRegularFile, container_path.string(),
        "Input path is not a regular file."));
    report.status = ValidationStatus::unreadable;
    report.core_status = ValidationStatus::unreadable;
    report.embedding_transport.status = "unreadable";
    return report;
  }

  const auto inspection = svp::package::inspect_embedded_svpi(container_path, true);
  report.embedding_transport.embedding_detected = !inspection.embeddings.empty();
  report.embedding_transport.container_supported = inspection.container.supported;
  report.embedding_transport.container_kind =
      svp::package::to_string(inspection.container.kind);
  report.embedding_transport.major_brand = inspection.container.major_brand;
  report.embedding_transport.compatible_brands =
      inspection.container.compatible_brands;
  if (!inspection.embeddings.empty()) {
    const auto& embedding = inspection.embeddings.front();
    auto& transport = report.embedding_transport;
    transport.profile_version = embedding.profile_version;
    transport.box_offset = embedding.box_offset;
    transport.box_size = embedding.box_size;
    transport.payload_offset = embedding.payload_offset;
    transport.payload_size = embedding.payload_size;
    transport.payload_hash_status = !embedding.hash_verified
                                        ? "not_verified"
                                        : embedding.hash_matches ? "match" : "mismatch";
  } else {
    report.embedding_transport.payload_hash_status = "not_present";
  }

  for (const auto& issue : inspection.issues) {
    add_finding(report, make_finding(
        registry, validation_code_for_embedded_issue(issue.code),
        "/iso_bmff/offset/" + std::to_string(issue.offset), issue.message));
  }

  const bool transport_valid = inspection.has_single_valid_embedding() &&
      inspection.embeddings.front().hash_verified &&
      inspection.embeddings.front().hash_matches && inspection.issues.empty();
  if (!transport_valid) {
    report.embedding_transport.status = transport_status(inspection);
    recompute_status(report);
    return report;
  }

  report.embedding_transport.status = "valid";
  SvpiValidatorOptions svpi_options;
  svpi_options.validation_codes_path = options.validation_codes_path;
  svpi_options.registry_root_path = options.registry_root_path;
  svpi_options.schema_root_path = options.schema_root_path;
  svpi_options.allow_embedded_transport = true;
  const auto embedded_report = validate_svpi_package(container_path, svpi_options);
  report.embedding_transport.embedded_package_status =
      to_string(embedded_report.status);
  merge_findings(report, embedded_report);
  if (embedded_report.core_status == ValidationStatus::invalid ||
      embedded_report.status == ValidationStatus::unreadable) {
    add_finding(report, make_finding(
        registry, kCodeIsoBmffEmbeddedSvpiInvalid, "/embedded_svpi",
        "The transport envelope is valid, but the embedded SVPI package is invalid."));
  }
  add_embedded_media_binding_finding(
      report, registry, container_path, inspection.embeddings.front());
  recompute_status(report);
  return report;
}

}  // namespace svp::validation
