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
};

[[nodiscard]] MediaIngestPlan build_media_ingest_plan(std::filesystem::path source_path,
                                                      MediaProbe probe);
[[nodiscard]] nlohmann::json media_ingest_plan_to_json(const MediaIngestPlan& plan);

}  // namespace svp::media
