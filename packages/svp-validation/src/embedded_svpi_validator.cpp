#include "svp/validation/embedded_svpi_validator.hpp"

#include "svp/core/version.hpp"
#include "svp/package/embedded_svpi.hpp"
#include "svp/package/svpi_embedding_profile.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/svpi_validator.hpp"

#include "embedded_media_binding_validation.hpp"

#include <exception>
#include <string_view>

namespace svp::validation {
namespace {

std::string_view issue_validation_code(svp::package::EmbeddedSvpiIssueCode code) {
  using Issue = svp::package::EmbeddedSvpiIssueCode;
  switch (code) {
    case Issue::input_unreadable:
    case Issue::invalid_box_structure:
      return kCodeMp4BoxStructureInvalid;
    case Issue::truncated_uuid_box:
      return kCodeMp4UuidBoxTruncated;
    case Issue::unsupported_profile_version:
      return kCodeMp4SvpiProfileUnsupported;
    case Issue::invalid_envelope_size:
    case Issue::unsupported_flags:
    case Issue::invalid_envelope_magic:
      return kCodeMp4SvpiEnvelopeInvalid;
    case Issue::payload_outside_file:
      return kCodeMp4SvpiPayloadBounds;
    case Issue::payload_length_mismatch:
      return kCodeMp4SvpiPayloadLengthMismatch;
    case Issue::payload_hash_mismatch:
      return kCodeMp4SvpiPayloadHashMismatch;
    case Issue::duplicate_embeddings:
      return kCodeMp4SvpiDuplicate;
    case Issue::no_embedding:
      return kCodeMp4SvpiNotFound;
    case Issue::unsafe_tail_layout:
      return kCodeMp4UnsafeTailLayout;
    case Issue::zero_sized_top_level_box:
      return kCodeMp4ZeroSizedBox;
    case Issue::output_exists:
    case Issue::output_write_failed:
      return kCodeMp4BoxStructureInvalid;
  }
  return kCodeMp4BoxStructureInvalid;
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

}  // namespace

std::string_view validation_code_for_embedded_issue(
    svp::package::EmbeddedSvpiIssueCode code) noexcept {
  return issue_validation_code(code);
}

ValidationReport validate_embedded_svpi_mp4(
    const std::filesystem::path& mp4_path,
    const EmbeddedSvpiValidatorOptions& options) {
  ValidationReport report;
  report.validator = {
      .name = "svp-validator",
      .version = std::string{svp::core::kToolVersion},
  };
  report.package_path = mp4_path.string();
  report.embedding_transport.present = true;
  report.embedding_transport.profile =
      std::string{svp::package::kSvpiMp4ProfileName};
  report.embedding_transport.uuid = std::string{svp::package::kSvpiMp4UuidText};

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

  const auto inspection = svp::package::inspect_embedded_svpi(mp4_path, true);
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
        "/mp4/offset/" + std::to_string(issue.offset), issue.message));
  }

  const bool transport_valid = inspection.has_single_valid_embedding() &&
      inspection.embeddings.front().hash_verified &&
      inspection.embeddings.front().hash_matches && inspection.issues.empty();
  if (!transport_valid) {
    report.embedding_transport.status = "invalid";
    recompute_status(report);
    return report;
  }

  report.embedding_transport.status = "valid";
  SvpiValidatorOptions svpi_options;
  svpi_options.validation_codes_path = options.validation_codes_path;
  svpi_options.registry_root_path = options.registry_root_path;
  svpi_options.schema_root_path = options.schema_root_path;
  svpi_options.allow_embedded_mp4 = true;
  const auto embedded_report = validate_svpi_package(mp4_path, svpi_options);
  report.embedding_transport.embedded_package_status =
      to_string(embedded_report.status);
  merge_findings(report, embedded_report);
  if (embedded_report.core_status == ValidationStatus::invalid ||
      embedded_report.status == ValidationStatus::unreadable) {
    add_finding(report, make_finding(
        registry, kCodeMp4EmbeddedSvpiInvalid, "/embedded_svpi",
        "The transport envelope is valid, but the embedded SVPI package is invalid."));
  }
  add_embedded_media_binding_finding(
      report, registry, mp4_path, inspection.embeddings.front());
  recompute_status(report);
  return report;
}

}  // namespace svp::validation
