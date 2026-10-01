#include "message_fields.hpp"

#include <limits>

namespace svp::exec::worker::detail {

std::string require_frame(const Frame& frame, MessageType expected, std::size_t payloads) {
  const std::string name(message_type_name(expected));
  if (frame.type != expected) {
    throw ExecError(ExecErrorCode::frame_malformed,
                    "expected a " + name + " frame, got " +
                        std::string(message_type_name(frame.type)));
  }
  if (frame.payloads.size() != payloads) {
    throw ExecError(ExecErrorCode::frame_malformed,
                    name + " frames carry " + std::to_string(payloads) + " payload(s), got " +
                        std::to_string(frame.payloads.size()));
  }
  std::string path = name + ".body";
  require_object(frame.body, path);
  return path;
}

std::uint32_t required_u32(const nlohmann::json& object, std::string_view name,
                           std::string_view path) {
  const std::uint64_t value = required_unsigned(object, name, path);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw ExecError(ExecErrorCode::invalid_value, child_path(path, name) + " is out of range");
  }
  return static_cast<std::uint32_t>(value);
}

nlohmann::json digests_to_json(const std::vector<Blake3Digest>& digests, bool prefixed) {
  nlohmann::json array = nlohmann::json::array();
  for (const Blake3Digest& digest : digests) {
    array.push_back(prefixed ? blake3_prefixed(digest) : blake3_hex(digest));
  }
  return array;
}

std::vector<Blake3Digest> required_digest_array(const nlohmann::json& object,
                                                std::string_view name, std::string_view path,
                                                bool prefixed) {
  const nlohmann::json& array = required_array(object, name, path);
  std::vector<Blake3Digest> digests;
  digests.reserve(array.size());
  for (const nlohmann::json& item : array) {
    if (!item.is_string()) {
      throw ExecError(ExecErrorCode::wrong_type,
                      child_path(path, name) + " must hold digest strings");
    }
    const std::string text = item.get<std::string>();
    const std::optional<Blake3Digest> digest =
        prefixed ? parse_blake3_prefixed(text) : parse_blake3_hex(text);
    if (!digest) {
      throw ExecError(ExecErrorCode::invalid_digest,
                      child_path(path, name) + " holds an invalid digest `" + text + "`");
    }
    digests.push_back(*digest);
  }
  return digests;
}

}  // namespace svp::exec::worker::detail
