#pragma once

// The size of an analysis working copy (svp-builder build --max-resolution).
//
// Nothing a build derives from frames needs more than 1080p: OCR decodes
// frames no longer than kOcrMaxFrameDimension (1920) on their long side,
// every other frame stage decodes at the 640-pixel canonical raster, and
// speech uses only the audio. A 4K source still costs its full resolution in
// every decode and in every byte sent to a worker Mac. With
// --max-resolution N, a primary video whose displayed short side is longer
// than N is analysed from a copy scaled so that side is N
// (working_copy.hpp); one at or below N is analysed as is. Bounding the short
// side treats landscape and portrait alike: 3840x2160 and 2160x3840 become
// 1920x1080 and 1080x1920, the frames OCR reads from either today.

#include "svp/media/canonical_raster.hpp"
#include "svp/media/media_probe.hpp"

#include <optional>

namespace svp::builder::working_copy {

// The smallest --max-resolution: the canonical analysis raster's longest
// side (svp/media/canonical_raster.hpp). A working copy whose short side is
// at least this long is never smaller than the raster every canonical frame
// is scaled to, in either dimension, whatever the video's aspect ratio.
inline constexpr int kMinimumMaxResolution = svp::media::kCanonicalLongestDisplayDimension;

struct WorkingCopyGeometry {
  // The frame size the encoder writes, in the source's stored orientation:
  // the copy keeps the source's rotation metadata.
  int stored_width = 0;
  int stored_height = 0;
  // That frame as displayed, rotation applied.
  int display_width = 0;
  int display_height = 0;
};

// nullopt when the video's displayed short side (rotation applied: a 90 or
// 270 degree rotation swaps the stored sides) is at most `max_resolution`:
// the source is analysed as is. Otherwise the copy's size: the short side
// `max_resolution` rounded down to an even number, the long side scaled by
// the same factor and rounded to the nearest even number (4:2:0 chroma needs
// even sides). Scaling both stored sides by one factor keeps the pixel aspect
// ratio, so the displayed aspect ratio is the source's up to that rounding.
[[nodiscard]] std::optional<WorkingCopyGeometry> plan_working_copy_geometry(
    const svp::media::VideoStreamProbe& video, int max_resolution);

}  // namespace svp::builder::working_copy
