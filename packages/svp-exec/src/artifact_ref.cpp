#include "svp/exec/artifact_ref.hpp"

#include "artifact_ref_json.hpp"
#include "json_fields.hpp"
#include "record_identifiers.hpp"
#include "svp/exec/exec_error.hpp"

#include <utility>

namespace svp::exec {

void validate_artifact_ref(const ArtifactRef& ref) {
  if (!detail::is_media_type(ref.media_type)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "artifact media_type must be a lowercase type/subtype: `" +
                        ref.media_type + "`");
  }
  if (!detail::is_lower_identifier(ref.role)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "artifact role must be a lowercase identifier: `" +
                        ref.role + "`");
  }
}

nlohmann::json artifact_ref_to_json(const ArtifactRef& ref) {
  validate_artifact_ref(ref);
  return nlohmann::json{{"blake3", blake3_hex(ref.blake3)},
                        {"bytes", ref.bytes},
                        {"media_type", ref.media_type},
                        {"role", ref.role}};
}

ArtifactRef artifact_ref_from_json(const nlohmann::json& value) {
  return detail::artifact_ref_from_json_at(value, "artifact_ref");
}

ArtifactRef make_artifact_ref(std::span<const std::byte> content,
                              std::string media_type, std::string role) {
  ArtifactRef ref{.blake3 = blake3_digest(content),
                  .bytes = content.size(),
                  .media_type = std::move(media_type),
                  .role = std::move(role)};
  validate_artifact_ref(ref);
  return ref;
}

namespace detail {

ArtifactRef artifact_ref_from_json_at(const nlohmann::json& value,
                                      std::string_view path) {
  require_object(value, path);
  reject_unknown_fields(value, {"blake3", "bytes", "media_type", "role"}, path);
  ArtifactRef ref{.blake3 = required_blake3_hex(value, "blake3", path),
                  .bytes = required_unsigned(value, "bytes", path),
                  .media_type = required_string(value, "media_type", path),
                  .role = required_string(value, "role", path)};
  validate_artifact_ref(ref);
  return ref;
}

}  // namespace detail
}  // namespace svp::exec
