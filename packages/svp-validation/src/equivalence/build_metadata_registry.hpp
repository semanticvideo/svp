#pragma once

#include <span>
#include <string_view>

namespace svp::validation::equivalence {

// Value written in place of a normalized field on both sides, so field
// presence stays exact-governed while the value is ignored.
inline constexpr std::string_view kNormalizedBuildMetadataPlaceholder =
    "<normalized build metadata>";

// Value predicates that narrow a normalization selector.
enum class BuildMetadataValueFilter {
  any_value,
  // A string that names a location on the build host's filesystem (POSIX
  // absolute path or drive-letter path). Package-relative references never
  // match, so they stay exact.
  absolute_host_path,
};

// A JSON field that is build metadata by construction: it records when or
// where a build ran rather than what the package observed. These fields are
// normalized only when EquivalenceOptions::normalize_build_metadata is set.
//
// `pointer` is a JSON Pointer applied to the document (JSON entries) or to
// every record (JSONL entries); a "*" segment matches any array index or
// object key.
struct JsonBuildMetadataField {
  std::string_view entry;
  std::string_view pointer;
  BuildMetadataValueFilter filter = BuildMetadataValueFilter::any_value;
  std::string_view reason;
};

// A SQLite row whose value column mirrors a JSON build-metadata field.
struct SqliteBuildMetadataRow {
  std::string_view table;
  std::string_view key_column;
  std::string_view key_text;
  std::string_view value_column;
  std::string_view reason;
};

[[nodiscard]] std::span<const JsonBuildMetadataField> json_build_metadata_fields();
[[nodiscard]] std::span<const SqliteBuildMetadataRow> sqlite_build_metadata_rows();

}  // namespace svp::validation::equivalence
