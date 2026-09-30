#pragma once

#include <span>
#include <string_view>

namespace svp::validation::equivalence {

// Where the hashed source of a digest field lives.
enum class DigestSourceKind {
  // The digest hashes one or more whole package layers named in `sources`.
  package_layers,
  // The digest hashes one SVPB block, located by the record's own
  // block_file and block_offset fields (or SQLite columns).
  record_block,
};

// A field that stores a BLAKE3 digest of another package layer. RC2
// Sections 5.16.2 and 17.5: hashes are never tolerance-compared, but a
// mismatch of a digest whose source layer passes its own equivalence rule
// does not by itself make packages not equivalent. The comparator reports
// such mismatches as hash_mismatch_allowed_by_source_layer_equivalence and
// otherwise as not_equivalent.
struct JsonDerivedDigestField {
  std::string_view entry;
  std::string_view pointer;
  DigestSourceKind kind = DigestSourceKind::package_layers;
  std::span<const std::string_view> sources;
  std::string_view reason;
};

struct SqliteDerivedDigestColumn {
  std::string_view table;
  std::string_view column;
  std::string_view block_file_column;
  std::string_view block_offset_column;
  std::string_view reason;
};

[[nodiscard]] std::span<const JsonDerivedDigestField> json_derived_digest_fields();
[[nodiscard]] std::span<const SqliteDerivedDigestColumn> sqlite_derived_digest_columns();

}  // namespace svp::validation::equivalence
