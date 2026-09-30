#pragma once

#include "equivalence/equivalence_ledger.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace svp::validation::equivalence {

enum class JsonEntryKind {
  json,
  jsonl,
};

// Canonical analysis raster of the left package, used to express normalized
// coordinates in pixels for pixel-denominated tolerances.
struct RasterSize {
  double width = 0.0;
  double height = 0.0;
};

struct JsonCompareSettings {
  bool normalize_build_metadata = false;
  std::optional<RasterSize> raster;
};

// Compares one JSON or JSONL entry whose bytes differ. Values are compared
// after canonicalization (UTF-8 parse, object key order, JSONL line endings
// and blank lines), exact except for Default Equivalence Profile v1 field
// rules, registered derived digests, and (when enabled) registered build
// metadata. Returns false when either side is not parseable JSON; the caller
// then compares bytes exactly.
[[nodiscard]] bool compare_json_entry(std::string_view entry,
                                      JsonEntryKind kind,
                                      const std::string& left_bytes,
                                      const std::string& right_bytes,
                                      const JsonCompareSettings& settings,
                                      EquivalenceLedger& ledger);

}  // namespace svp::validation::equivalence
