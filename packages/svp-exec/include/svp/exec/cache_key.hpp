#pragma once

#include "svp/exec/blake3_digest.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace svp::exec {

// RC2 §20.3 domain string; the first element of every cache key.
inline constexpr std::string_view kCacheKeyDomain = "svp-cache-key-v1";

// RC2 §20.3 writes the cache key as BLAKE3("svp-cache-key-v1", field, ...)
// without fixing how the fields are joined. svp-exec fixes it as:
//
//   material  = encode_canonical_json([ "svp-cache-key-v1", f1, f2, ..., fn ])
//   cache_key = BLAKE3(material), written as "b3:<hex>" (RC2 §20.2).
//
// A canonical JSON array is self-delimiting (no field can bleed into its
// neighbour), keeps each field's type (a string "30" never equals the integer
// 30), and the material can be recorded verbatim in provenance as RC2 §5.15
// rule 4 requires. Field order is the caller's contract: pass the fields in
// the order the processor's cache-key definition lists them.
// `ordered_fields` must be a JSON array; its elements may be any canonical
// JSON value.
[[nodiscard]] std::string cache_key_material(const nlohmann::json& ordered_fields);
[[nodiscard]] Blake3Digest compute_cache_key(const nlohmann::json& ordered_fields);

}  // namespace svp::exec
