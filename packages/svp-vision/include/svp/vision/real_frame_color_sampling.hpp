#pragma once

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/color_frame_sampling.hpp"

#include <filesystem>
#include <string>

namespace svp::vision {

// Result of attempting to sample real decoded frames from the source media.
// When real_decoding_attempted is false, the input field contains the
// synthetic fallback and no frames were decoded.  When true, frames were
// decoded and scaled to the canonical raster from the actual source.
struct RealFrameSamplingResult {
  ColorFrameSamplingInput input;
  bool real_decoding_attempted = false;
  bool real_decoding_succeeded = false;
  std::string skipped_reason;
};

// Build a ColorFrameSamplingInput by decoding real frames from the source media
// at deterministic timestamps using ffmpeg.  The frames are scaled to the
// canonical analysis raster computed from the MediaIngestPlan.
//
// If ffmpeg is not available, or if decoding fails, the function returns a
// result with real_decoding_attempted=false and a synthetic fallback input
// identical to build_foundation_color_staging_sample_input().
//
// The canonical raster dimensions (plan.canonical_raster.width / .height)
// are used as the decoding target size so that pixel coordinates are
// consistent with the spec canonical-raster boundary.
[[nodiscard]] RealFrameSamplingResult build_real_frame_color_sampling_input(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path);

}  // namespace svp::vision
