#pragma once

#include "equivalence/equivalence_ledger.hpp"

#include <filesystem>
#include <string_view>

namespace svp::validation::equivalence {

// Source-layer-aware logical comparison of two index/index.sqlite entries
// (RC2 Section 17.5). Raw SQLite bytes are never compared: both databases
// are read through the canonical logical row stream, rows are matched by
// their exact-class columns, and the remaining columns use the Section 17.5
// numerical or source-payload-hash handling.
void compare_index_entry(std::string_view entry,
                         const std::filesystem::path& left_package,
                         const std::filesystem::path& right_package,
                         bool normalize_build_metadata,
                         EquivalenceLedger& ledger);

}  // namespace svp::validation::equivalence
