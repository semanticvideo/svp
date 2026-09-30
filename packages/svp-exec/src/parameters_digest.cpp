#include "svp/exec/parameters_digest.hpp"

#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"

namespace svp::exec {

Blake3Digest compute_parameters_blake3(const nlohmann::json& parameters) {
  if (!parameters.is_object()) {
    throw ExecError(ExecErrorCode::wrong_type,
                    "task parameters must be a JSON object");
  }
  return blake3_digest(encode_canonical_json(parameters));
}

}  // namespace svp::exec
