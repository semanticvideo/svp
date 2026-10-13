#pragma once

// The size of an analysis working copy (svp-builder build --max-height).
//
// Nothing a build derives from frames needs more than about 1080 lines: OCR
// decodes frames no wider than kOcrMaxFrameDimension (1920), every other
// frame stage decodes at the 640-pixel canonical raster, and speech uses only
// the audio. A 4K source still costs its full resolution in every decode and
// in every byte sent to a worker Mac. With --max-height N, a primary video
// displayed taller than N is analysed from a copy scaled to fit N
// (working_copy.hpp); one at or below N is analysed as is.

#include "svp/media/canonical_raster.hpp"
#include "svp/media/media_probe.hpp"

#include <optional>

namespace svp::builder::working_copy {

// The smallest --max-height: the canonical analysis raster's longest side
// (svp/media/canonical_raster.hpp). A working copy at least this tall is
// never smaller than the raster every canonical frame is scaled to, whatever
// the video's aspect ratio.
inline constexpr int kMinimumMaxHeight = svp::media::kCanonicalLongestDisplayDimension;

struct WorkingCopyGeometry {
  // The frame size the encoder writes, in the source's stored orientation:
  // the copy keeps the source's rotation metadata.
  int stored_width = 0;
  int stored_height = 0;
  // That frame as displayed, rotation applied.
  int display_width = 0;
  int display_height = 0;
};

// nullopt when the video's displayed height (its stored width under a 90 or
// 270 degree rotation) is at most `max_height`: the source is analysed as is.
// Otherwise the copy's size: displayed height `max_height` rounded down to an
// even number, the other side scaled by the same factor and rounded to the
// nearest even number (4:2:0 chroma needs even sides). Scaling both stored
// sides by one factor keeps the pixel aspect ratio, so the displayed aspect
// ratio is the source's up to that rounding.
[[nodiscard]] std::optional<WorkingCopyGeometry> plan_working_copy_geometry(
    const svp::media::VideoStreamProbe& video, int max_height);

}  // namespace svp::builder::working_copy
