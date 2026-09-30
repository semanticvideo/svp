#pragma once

#include <string_view>

namespace svp::validation::equivalence {

// RC2 Section 17.5 cross-package equivalence classes for SQLite columns.
enum class SqliteColumnClass {
  // Table names, schema fingerprints, IDs, refs, time/frame ranges, block
  // placement, relationship rows, FTS text, and every column not assigned
  // below.
  exact,
  // REAL confidence columns and columns whose source layer is
  // tolerance-governed (Section 5.16.1: SQLite keeps the source value's
  // equivalence class). Compared by absolute difference.
  numeric_tolerance,
  // vector_index.embedding: cosine similarity of float32 vectors.
  embedding_vector,
  // binary_blocks hash columns: decided by the referenced block payload.
  derived_block_digest,
};

struct SqliteColumnRule {
  SqliteColumnClass column_class = SqliteColumnClass::exact;
  std::string_view rule_id;
  double tolerance = 0.0;
};

[[nodiscard]] SqliteColumnRule sqlite_column_rule(std::string_view table,
                                                  std::string_view column);

}  // namespace svp::validation::equivalence
