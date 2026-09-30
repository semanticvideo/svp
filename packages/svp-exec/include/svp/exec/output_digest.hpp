#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/blake3_digest.hpp"

#include <span>
#include <string>
#include <string_view>

namespace svp::exec {

// Domain string that keeps an output digest from ever colliding with the
// BLAKE3 of some other canonical JSON document.
inline constexpr std::string_view kOutputDigestDomain = "svp-task-output-digest-v1";

// `output_digest` byte construction:
//   material = encode_canonical_json(
//                [ "svp-task-output-digest-v1",
//                  [ artifact_ref_to_json(outputs[0]), ..., (outputs[n-1]) ] ])
//   output_digest = BLAKE3(material), written as "b3:<hex>" in TaskResult.
// Output order is significant (it is the task's canonical output order), so
// the same refs in a different order produce a different digest. An empty
// output list is valid and has a fixed digest.
[[nodiscard]] std::string output_digest_material(
    std::span<const ArtifactRef> outputs);
[[nodiscard]] Blake3Digest compute_output_digest(
    std::span<const ArtifactRef> outputs);

}  // namespace svp::exec
