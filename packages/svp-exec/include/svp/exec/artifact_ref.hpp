#pragma once

#include "svp/exec/blake3_digest.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>

namespace svp::exec {

// Content-addressed reference to anything that crosses a process boundary
// (plan §4.2). JSON form:
//   {"blake3":"<64 hex>","bytes":<uint>,"media_type":"type/subtype","role":"<id>"}
// All four fields are required.
struct ArtifactRef {
  Blake3Digest blake3{};
  std::uint64_t bytes = 0;
  std::string media_type;
  std::string role;

  bool operator==(const ArtifactRef&) const = default;
};

// Throws ExecError(invalid_value) when media_type or role break the wire
// identifier rules.
void validate_artifact_ref(const ArtifactRef& ref);

[[nodiscard]] nlohmann::json artifact_ref_to_json(const ArtifactRef& ref);
[[nodiscard]] ArtifactRef artifact_ref_from_json(const nlohmann::json& value);

// Describes `content` by its BLAKE3 digest and length.
[[nodiscard]] ArtifactRef make_artifact_ref(std::span<const std::byte> content,
                                            std::string media_type,
                                            std::string role);

}  // namespace svp::exec
