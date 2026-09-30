#pragma once

// Path-aware ArtifactRef decoding shared by the record codecs so rejection
// messages name the exact field ("task_spec.inputs.source.blake3").

#include "svp/exec/artifact_ref.hpp"

#include <nlohmann/json.hpp>
#include <string_view>

namespace svp::exec::detail {

[[nodiscard]] ArtifactRef artifact_ref_from_json_at(const nlohmann::json& value,
                                                    std::string_view path);

}  // namespace svp::exec::detail
