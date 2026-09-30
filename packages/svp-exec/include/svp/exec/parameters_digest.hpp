#pragma once

#include "svp/exec/blake3_digest.hpp"

#include <nlohmann/json.hpp>

namespace svp::exec {

// `parameters_blake3` (RC2 §20.2, plan §4.1):
//   BLAKE3( encode_canonical_json(parameters) )
// over the UTF-8 bytes of the canonical encoding, with no prefix or domain
// string, so the value equals the BLAKE3 of the `parameters` member exactly as
// it appears inside a canonical TaskSpec. `parameters` must be a JSON object.
[[nodiscard]] Blake3Digest compute_parameters_blake3(
    const nlohmann::json& parameters);

}  // namespace svp::exec
