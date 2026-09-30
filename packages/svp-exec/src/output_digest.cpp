#include "svp/exec/output_digest.hpp"

#include "svp/exec/canonical_json.hpp"

namespace svp::exec {

std::string output_digest_material(std::span<const ArtifactRef> outputs) {
  nlohmann::json refs = nlohmann::json::array();
  for (const ArtifactRef& output : outputs) {
    refs.push_back(artifact_ref_to_json(output));
  }
  return encode_canonical_json(
      nlohmann::json::array({std::string(kOutputDigestDomain), std::move(refs)}));
}

Blake3Digest compute_output_digest(std::span<const ArtifactRef> outputs) {
  return blake3_digest(output_digest_material(outputs));
}

}  // namespace svp::exec
