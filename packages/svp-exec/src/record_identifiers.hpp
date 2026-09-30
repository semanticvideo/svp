#pragma once

// Character rules for the identifier-like strings carried by svp-exec
// records. These are wire-format rules; changing them changes which records
// a runtime accepts.

#include <string_view>

namespace svp::exec::detail {

// Task IDs and session IDs: non-empty ASCII [A-Za-z0-9._-]. RC2 §20.2 task IDs
// ("task.depth.shot_000421.frames_00120480_00121920") and the plan's session
// IDs ("bs_…", "ws_…") fit; whitespace, separators, and non-ASCII do not.
[[nodiscard]] bool is_record_identifier(std::string_view value) noexcept;

// Task type names, artifact roles, input names, and error codes: non-empty
// lowercase ASCII [a-z0-9._] ("ocr.frame_batch", "source_media").
[[nodiscard]] bool is_lower_identifier(std::string_view value) noexcept;

// Media types without parameters, lowercase, per the RFC 6838 §4.2
// restricted-name grammar: "type/subtype" ("video/mp4",
// "application/x-ndjson").
[[nodiscard]] bool is_media_type(std::string_view value) noexcept;

}  // namespace svp::exec::detail
