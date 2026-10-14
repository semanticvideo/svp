#pragma once

// The size of the resized media of svp-builder --max-resolution
// (resized_source.hpp).
//
// With --max-resolution N, a primary video whose displayed short side is
// longer than N is resized so that side is N, and the build describes the
// resized media; one at or below N is built as is. Bounding the short side
// treats landscape and portrait alike: 3840x2160 and 2160x3840 become
// 1920x1080 and 1080x1920.

#include "svp/media/canonical_raster.hpp"
#include "svp/media/media_probe.hpp"

#include <optional>

namespace svp::builder::resize {

// The smallest --max-resolution: the canonical analysis raster's longest
// side (svp/media/canonical_raster.hpp). Resized media whose short side is at
// least this long is never smaller than the raster every canonical frame is
// scaled to, in either dimension, whatever the video's aspect ratio.
inline constexpr int kMinimumMaxResolution = svp::media::kCanonicalLongestDisplayDimension;

struct ResizeGeometry {
  // The frame size the encoder writes, in the source's stored orientation:
  // the resized media keeps the source's rotation metadata.
  int stored_width = 0;
  int stored_height = 0;
  // That frame as displayed, rotation applied.
  int display_width = 0;
  int display_height = 0;
};

// nullopt when the video's displayed short side (rotation applied: a 90 or
// 270 degree rotation swaps the stored sides) is at most `max_resolution`:
// the source is built as is. Otherwise the resized size: the short side
// `max_resolution` rounded down to an even number, the long side scaled by
// the same factor and rounded to the nearest even number (4:2:0 chroma needs
// even sides). Scaling both stored sides by one factor keeps the pixel aspect
// ratio, so the displayed aspect ratio is the source's up to that rounding.
[[nodiscard]] std::optional<ResizeGeometry> plan_resize_geometry(
    const svp::media::VideoStreamProbe& video, int max_resolution);

}  // namespace svp::builder::resize
