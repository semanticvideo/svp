#pragma once

// Strict field access for svp-exec record decoding. Every accessor throws
// ExecError with a code that names the violated rule, and messages carry the
// dotted field path so a rejected record is diagnosable from the error alone.

#include "svp/exec/blake3_digest.hpp"

#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace svp::exec::detail {

void require_object(const nlohmann::json& value, std::string_view path);

// Rejects any member of `object` not listed in `allowed`.
void reject_unknown_fields(const nlohmann::json& object,
                           std::initializer_list<std::string_view> allowed,
                           std::string_view path);

[[nodiscard]] const nlohmann::json& required_field(const nlohmann::json& object,
                                                   std::string_view name,
                                                   std::string_view path);

[[nodiscard]] std::string required_string(const nlohmann::json& object,
                                          std::string_view name,
                                          std::string_view path);

[[nodiscard]] std::uint64_t required_unsigned(const nlohmann::json& object,
                                              std::string_view name,
                                              std::string_view path);

[[nodiscard]] bool required_bool(const nlohmann::json& object,
                                 std::string_view name,
                                 std::string_view path);

[[nodiscard]] const nlohmann::json& required_object(const nlohmann::json& object,
                                                    std::string_view name,
                                                    std::string_view path);

[[nodiscard]] const nlohmann::json& required_array(const nlohmann::json& object,
                                                   std::string_view name,
                                                   std::string_view path);

// Bare 64-character lowercase hex (RC2 §5.15 rule 2).
[[nodiscard]] Blake3Digest required_blake3_hex(const nlohmann::json& object,
                                               std::string_view name,
                                               std::string_view path);

// "b3:" + 64-character lowercase hex (RC2 §20.2 cache_key form).
[[nodiscard]] Blake3Digest required_blake3_prefixed(const nlohmann::json& object,
                                                    std::string_view name,
                                                    std::string_view path);

[[nodiscard]] std::string child_path(std::string_view path, std::string_view name);

}  // namespace svp::exec::detail
