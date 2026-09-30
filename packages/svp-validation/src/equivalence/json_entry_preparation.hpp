#pragma once

#include "equivalence/equivalence_ledger.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace svp::validation::equivalence {

// Replaces registered build-metadata fields of one record pair with a shared
// placeholder and reports each normalized field. Only called when
// EquivalenceOptions::normalize_build_metadata is set.
void normalize_build_metadata(std::string_view entry,
                              nlohmann::json& left,
                              nlohmann::json& right,
                              const std::string& location_prefix,
                              EquivalenceLedger& ledger);

// For registered derived-digest fields whose values differ, defers the
// decision to the hashed source layers and replaces both values with a
// shared placeholder so the structural diff does not report them twice.
void defer_derived_digests(std::string_view entry,
                           nlohmann::json& left,
                           nlohmann::json& right,
                           const std::string& location_prefix,
                           EquivalenceLedger& ledger);

}  // namespace svp::validation::equivalence
