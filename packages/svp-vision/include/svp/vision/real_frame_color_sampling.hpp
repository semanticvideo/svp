#pragma once

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/frame_catalog.hpp"

#include <filesystem>
#include <string>

namespace svp::vision {

// Result of attempting to sample real decoded frames from the source media.
//
// real_decoding_attempted - true when ffmpeg was found and pre-conditions were
//   met (duration known, canonical raster positive).  False means no decode
//   was tried at all and the synthetic fallback was used directly.
//
// real_decoding_succeeded - true when at least one frame was successfully
//   decoded from the source.  Always false when real_decoding_attempted is
//   false.
//
// frames_attempted  - how many timestamps were presented to ffmpeg
// frames_decoded    - how many of those produced a full pixel buffer
// frames_missed     - frames_attempted - frames_decoded
//
// input contains whatever frame/scene/shot data should be fed to the color
// pipeline.  When real_decoding_succeeded is true this is real canonical
// raster data.  Otherwise it is the synthetic 2x2 fallback.
struct RealFrameSamplingResult {
  ColorFrameSamplingInput input;
  bool real_decoding_attempted = false;
  bool real_decoding_succeeded = false;
  int frames_attempted = 0;
  int frames_decoded = 0;
  int frames_missed = 0;
  std::string skipped_reason;
};

// Build a ColorFrameSamplingInput by decoding real frames from the source media
// at deterministic timestamps using ffmpeg.  The frames are scaled to the
// canonical analysis raster computed from the MediaIngestPlan.
//
// Individual per-frame decode misses are tolerated: if some timestamps fail
// but at least one frame is decoded the result uses only the successfully
// decoded frames.  real_decoding_succeeded and the frames_decoded/missed
// counters in the result reflect the actual outcome.
//
// If ffmpeg is not available, if the duration is unknown, or if every targeted
// frame fails, the function falls back to the synthetic sample input from
// build_foundation_color_staging_sample_input() and sets
// real_decoding_attempted and real_decoding_succeeded accordingly.
[[nodiscard]] RealFrameSamplingResult build_real_frame_color_sampling_input(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path,
    FrameCatalog* frame_catalog = nullptr,
    FrameProgressCallback on_progress = {});

}  // namespace svp::vision
