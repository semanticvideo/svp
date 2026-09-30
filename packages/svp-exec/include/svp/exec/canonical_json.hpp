#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace svp::exec {

// Canonical JSON used by every svp-exec record and frame header (plan §4.2,
// the same canonicalization RC2 §5.16.2 requires for package JSON):
//
//   * UTF-8, emitted verbatim (no \u escapes except for control characters,
//     which use nlohmann's fixed lowercase \u00xx form);
//   * object keys sorted by unsigned byte order, which for UTF-8 equals code
//     point order;
//   * no insignificant whitespace;
//   * integers as plain decimal (no sign on zero, no exponent, no leading
//     zeros); floating-point values in nlohmann's shortest round-trip form;
//   * NaN and +/-Infinity are rejected, never written as `null`.
//
// Typed record fields that carry time, indices, sizes, or counts are unsigned
// integers; floating-point values only appear inside opaque `parameters` and
// `diagnostics` objects.

// Container nesting bound. Encoding and validation recurse over the DOM, so a
// hostile or corrupt peer must not be able to exhaust the stack. The fixed
// svp-exec record structure nests five levels (frame header > body > task
// spec > inputs > artifact ref); 64 leaves generous room for task-defined
// `parameters` and `diagnostics` objects.
inline constexpr std::size_t kMaxCanonicalJsonDepth = 64;

// Throws ExecError(non_finite_number) for NaN/Inf, (wrong_type) for binary or
// discarded values, (invalid_value) for nesting deeper than
// kMaxCanonicalJsonDepth, and (invalid_json) for strings that are not UTF-8.
[[nodiscard]] std::string encode_canonical_json(const nlohmann::json& value);

// Parses `bytes` and requires them to be exactly the canonical encoding of the
// parsed value. Throws ExecError(invalid_json) for malformed JSON,
// (invalid_value) for excessive nesting, and (non_canonical_json) when the
// bytes differ from their canonical form (whitespace, key order, duplicate
// keys, escapes, number spelling).
[[nodiscard]] nlohmann::json decode_canonical_json(std::string_view bytes);

}  // namespace svp::exec
