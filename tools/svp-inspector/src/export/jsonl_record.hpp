#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace package_export {

// Parses one trimmed JSONL record. Throws ExportError(malformed_record) when
// it is not JSON or not a JSON object, and ExportError(reserved_member_present)
// when the package record already uses the export's reserved member.
[[nodiscard]] nlohmann::json parse_record(std::string_view record_text,
                                          std::string_view entry,
                                          std::uint64_t line);

// The exported line for a record (Package_Export_v1.md Section 4.1): the
// original record text, byte for byte, with `"svp_export":<resolutions>`
// appended as its last member. `record_text` must be a trimmed JSON object
// and `resolutions` a non-empty object.
[[nodiscard]] std::string append_export_member(std::string_view record_text,
                                               const nlohmann::json& record,
                                               const nlohmann::json& resolutions);

}  // namespace package_export
