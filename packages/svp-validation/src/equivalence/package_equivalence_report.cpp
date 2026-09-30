#include "svp/validation/package_equivalence.hpp"

namespace svp::validation {

std::string_view to_string(EquivalenceClass value) noexcept {
  switch (value) {
    case EquivalenceClass::byte_identical:
      return "byte_identical";
    case EquivalenceClass::structurally_equivalent:
      return "structurally_equivalent";
    case EquivalenceClass::numerically_equivalent:
      return "numerically_equivalent";
    case EquivalenceClass::not_equivalent:
      return "not_equivalent";
  }
  return "not_equivalent";
}

std::string_view to_string(EquivalenceOutcome value) noexcept {
  switch (value) {
    case EquivalenceOutcome::not_equivalent:
      return "not_equivalent";
    case EquivalenceOutcome::within_tolerance:
      return "within_tolerance";
    case EquivalenceOutcome::hash_mismatch_allowed_by_source_layer_equivalence:
      return "hash_mismatch_allowed_by_source_layer_equivalence";
    case EquivalenceOutcome::normalized_build_metadata:
      return "normalized_build_metadata";
    case EquivalenceOutcome::canonicalization_only:
      return "canonicalization_only";
  }
  return "not_equivalent";
}

void to_json(nlohmann::json& json, const EquivalenceFinding& finding) {
  json = nlohmann::json{
      {"outcome", to_string(finding.outcome)},
      {"entry", finding.entry},
      {"layer", finding.layer},
      {"rule", finding.rule},
      {"location", finding.location},
      {"detail", finding.detail},
      {"occurrences", finding.occurrences},
  };
  if (finding.measured.has_value()) {
    json["measured"] = *finding.measured;
  }
  if (finding.tolerance.has_value()) {
    json["tolerance"] = *finding.tolerance;
  }
}

void to_json(nlohmann::json& json, const EquivalenceReport& report) {
  const auto identity = [](const EquivalencePackageIdentity& package) {
    return nlohmann::json{
        {"path", package.path},
        {"blake3", package.blake3},
        {"size_bytes", package.size_bytes},
    };
  };
  json = nlohmann::json{
      {"schema_version", "svp-equivalence-report-v1"},
      {"classification", to_string(report.classification)},
      {"profile", report.profile},
      {"normalize_build_metadata", report.normalize_build_metadata},
      {"left", identity(report.left)},
      {"right", identity(report.right)},
      {"entries_compared", report.entries_compared},
      {"findings", report.findings},
      {"exact_fallbacks", report.exact_fallbacks},
  };
}

}  // namespace svp::validation
