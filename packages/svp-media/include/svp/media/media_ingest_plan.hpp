#pragma once

#include "svp/media/canonical_raster.hpp"
#include "svp/media/media_probe.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::media {

struct MediaIngestPlan {
  std::filesystem::path source_path;
  MediaProbe probe;
  VideoStreamProbe primary_video_stream;
  CanonicalAnalysisRaster canonical_raster;
  // The file frame decoders read instead of source_path: a working copy of
  // the primary video at a lower resolution (svp-builder build --max-height)
  // with the same frames at the same timestamps and the same rotation. Empty
  // means decoders read source_path. Everything else in the plan (probe,
  // timing, dimensions, raster) still describes source_path, and
  // media_ingest_plan_to_json leaves this path out.
  std::filesystem::path frame_decode_path;
};

[[nodiscard]] MediaIngestPlan build_media_ingest_plan(std::filesystem::path source_path,
                                                      MediaProbe probe);
[[nodiscard]] nlohmann::json media_ingest_plan_to_json(const MediaIngestPlan& plan);

// The file whose frames analysis decodes: frame_decode_path when set,
// otherwise source_path. Every frame decoder reads this path.
[[nodiscard]] const std::filesystem::path& frame_source_path(const MediaIngestPlan& plan);

// True when frames are decoded from a working copy rather than the source.
[[nodiscard]] bool decodes_working_copy(const MediaIngestPlan& plan);

}  // namespace svp::media
