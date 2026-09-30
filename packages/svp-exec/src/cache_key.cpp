#include "svp/exec/cache_key.hpp"

#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"

namespace svp::exec {

std::string cache_key_material(const nlohmann::json& ordered_fields) {
  if (!ordered_fields.is_array()) {
    throw ExecError(ExecErrorCode::wrong_type,
                    "cache key fields must be a JSON array");
  }
  nlohmann::json material = nlohmann::json::array({std::string(kCacheKeyDomain)});
  for (const nlohmann::json& field : ordered_fields) {
    material.push_back(field);
  }
  return encode_canonical_json(material);
}

Blake3Digest compute_cache_key(const nlohmann::json& ordered_fields) {
  return blake3_digest(cache_key_material(ordered_fields));
}

}  // namespace svp::exec
