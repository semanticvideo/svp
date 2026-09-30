#pragma once

// Cross-package equivalence comparison (SVP RC2 Sections 5.16.1, 5.16.2,
// and 17.5), exposed by `svp-validator validate --equivalent <A> <B>`.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::validation {

// RC2 Section 5.16.2 reporting classes, strongest first.
enum class EquivalenceClass {
  byte_identical,
  structurally_equivalent,
  numerically_equivalent,
  not_equivalent,
};

struct EquivalenceOptions {
  // Opt-in: remove the fields listed in the build-metadata normalization
  // registry (wall-clock timestamps and host filesystem locations) from both
  // packages before comparing. Every normalized field is reported. When
  // false, those fields are compared exactly like any other field.
  bool normalize_build_metadata = false;
};

// Outcome of one finding. Only `not_equivalent` findings make the packages
// not equivalent; `within_tolerance` findings make them at most numerically
// equivalent; the rest are informational.
enum class EquivalenceOutcome {
  not_equivalent,
  within_tolerance,
  hash_mismatch_allowed_by_source_layer_equivalence,
  normalized_build_metadata,
  canonicalization_only,
};

struct EquivalenceFinding {
  EquivalenceOutcome outcome = EquivalenceOutcome::not_equivalent;
  std::string entry;     // package entry path, for example "manifest.json"
  std::string layer;     // comparison layer, for example "canonical_jsonl"
  std::string rule;      // rule applied, for example "confidence_fields"
  std::string location;  // first differing record/field inside the entry
  std::string detail;
  std::optional<double> measured;
  std::optional<double> tolerance;
  // Total occurrences folded into this finding (first location is kept).
  std::uint64_t occurrences = 1;
};

struct EquivalencePackageIdentity {
  std::string path;
  std::string blake3;
  std::uint64_t size_bytes = 0;
};

struct EquivalenceReport {
  EquivalenceClass classification = EquivalenceClass::not_equivalent;
  std::string profile;  // "svp-default-equivalence-profile-v1"
  bool normalize_build_metadata = false;
  EquivalencePackageIdentity left;
  EquivalencePackageIdentity right;
  std::uint64_t entries_compared = 0;
  std::vector<EquivalenceFinding> findings;
  // Standing profile notes: payload kinds whose Section 5.16.2 rule is not
  // evaluated by this validator and are therefore compared exactly.
  std::vector<std::string> exact_fallbacks;
};

[[nodiscard]] EquivalenceReport compare_packages(
    const std::filesystem::path& left,
    const std::filesystem::path& right,
    const EquivalenceOptions& options);

[[nodiscard]] std::string_view to_string(EquivalenceClass value) noexcept;
[[nodiscard]] std::string_view to_string(EquivalenceOutcome value) noexcept;

// 0 for byte_identical, structurally_equivalent, or numerically_equivalent;
// 1 for not_equivalent.
[[nodiscard]] int exit_code(const EquivalenceReport& report) noexcept;

void to_json(nlohmann::json& json, const EquivalenceFinding& finding);
void to_json(nlohmann::json& json, const EquivalenceReport& report);

}  // namespace svp::validation
