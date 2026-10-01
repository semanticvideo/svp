#pragma once

// Field helpers shared by the worker protocol codecs. They build on the
// svp-exec JSON field rules (json_fields.hpp) and throw the same ExecErrors.

#include "json_fields.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/frame.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker::detail {

using svp::exec::detail::child_path;
using svp::exec::detail::reject_unknown_fields;
using svp::exec::detail::require_object;
using svp::exec::detail::required_array;
using svp::exec::detail::required_blake3_hex;
using svp::exec::detail::required_blake3_prefixed;
using svp::exec::detail::required_bool;
using svp::exec::detail::required_field;
using svp::exec::detail::required_object;
using svp::exec::detail::required_string;
using svp::exec::detail::required_unsigned;

// Checks the frame type, that it carries exactly `payloads` payloads, and
// that its body is an object; returns "<TYPE>.body" for error paths.
std::string require_frame(const Frame& frame, MessageType expected, std::size_t payloads);

std::uint32_t required_u32(const nlohmann::json& object, std::string_view name,
                           std::string_view path);

nlohmann::json digests_to_json(const std::vector<Blake3Digest>& digests, bool prefixed);
std::vector<Blake3Digest> required_digest_array(const nlohmann::json& object,
                                                std::string_view name, std::string_view path,
                                                bool prefixed);

}  // namespace svp::exec::worker::detail
