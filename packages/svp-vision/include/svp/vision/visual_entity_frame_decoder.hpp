#pragma once

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/canonical_frame_input.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace svp::vision {

// Decode one bounded, uniformly sampled visual-entity window through a single
// ffmpeg process. The caller owns the exact timestamp plan; decoded frames are
// assigned to those timestamps in presentation order.
[[nodiscard]] DecodedCanonicalFrames decode_visual_entity_window(
    const media::MediaIngestPlan& media_plan,
    const std::filesystem::path& ffmpeg_path,
    int width,
    int height,
    const std::vector<std::int64_t>& timestamps_us,
    FrameCatalog* frame_catalog = nullptr,
    const std::string& purpose = "visual_entity_tracking");

}  // namespace svp::vision
