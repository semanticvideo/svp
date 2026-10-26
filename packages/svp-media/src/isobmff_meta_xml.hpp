#pragma once

// The XML an ISO BMFF file carries in its file-level metadata: the `xml ` box
// of the top-level `meta` box (ISO/IEC 14496-12, 8.11.2), where Sony cameras
// write their NonRealTimeMeta.

#include <filesystem>
#include <optional>
#include <string>

namespace svp::media {

// The XML text, or none when the file isn't ISO BMFF, has no top-level `meta`
// box with an `xml ` box, or its boxes are malformed. Only box headers are
// read until the `meta` box is found, so the media data is never loaded.
[[nodiscard]] std::optional<std::string> read_isobmff_meta_xml(
    const std::filesystem::path& path);

}  // namespace svp::media
