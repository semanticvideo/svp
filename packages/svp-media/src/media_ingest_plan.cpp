#include "svp/media/media_ingest_plan.hpp"

#include "svp/media/canonical_timing.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::media {
namespace {

nlohmann::json canonical_raster_to_json(const CanonicalAnalysisRaster& raster,
                                        const VideoStreamProbe& source) {
  return {
      {"width", raster.width},
      {"height", raster.height},
      {"derivation", "longest_display_dimension_640_preserve_dar"},
      {"source_stored_width", source.width},
      {"source_stored_height", source.height},
      {"source_display_width_units", raster.display.display_width_units},
      {"source_display_height_units", raster.display.display_height_units},
      {"source_display_aspect_ratio", raster.display.display_aspect_ratio},
      {"rotation_degrees_applied", source.rotation_degrees},
      {"pixel_aspect_ratio", format_rational(source.pixel_aspect_ratio)},
      {"pixel_aspect_ratio_applied", source.pixel_aspect_ratio.numerator !=
                                         source.pixel_aspect_ratio.denominator},
      {"coordinate_origin", "top_left"},
      {"normalized_coordinates", true},
  };
}

nlohmann::json frame_samples_to_json(const VideoStreamProbe& stream) {
  nlohmann::json samples = nlohmann::json::array();
  if (!stream.timing.average_frame_rate.has_value() ||
      !stream.timing.frame_count.has_value()) {
    return samples;
  }

  const std::int64_t sample_count = std::min<std::int64_t>(*stream.timing.frame_count, 5);
  for (std::int64_t index = 0; index < sample_count; ++index) {
    const std::int64_t pts_us =
        frame_index_to_microseconds(index, *stream.timing.average_frame_rate);
    samples.push_back({
        {"frame_index", index},
        {"pts_us", pts_us},
        {"pts_sec", microseconds_to_seconds_string(pts_us)},
    });
  }
  return samples;
}

}  // namespace

MediaIngestPlan build_media_ingest_plan(std::filesystem::path source_path,
                                        MediaProbe probe) {
  if (probe.video_streams.empty()) {
    throw std::runtime_error("media probe does not contain a video stream");
  }

  VideoStreamProbe primary_video_stream = probe.video_streams.front();
  CanonicalAnalysisRaster raster = compute_canonical_analysis_raster(SourceDisplayGeometry{
      primary_video_stream.width,
      primary_video_stream.height,
      primary_video_stream.rotation_degrees,
      primary_video_stream.pixel_aspect_ratio,
  });

  return MediaIngestPlan{
      std::move(source_path),
      std::move(probe),
      std::move(primary_video_stream),
      std::move(raster),
  };
}

nlohmann::json media_ingest_plan_to_json(const MediaIngestPlan& plan) {
  return {
      {"schema_version", "svp-builder-media-ingest-foundation-v1"},
      {"source", {{"path", plan.source_path.string()}}},
      {"primary_media_id", "media_0001"},
      {"primary_video_stream_id", plan.primary_video_stream.id},
      {"timebase",
       {{"unit", "microseconds"},
        {"origin", "primary_presentation_start"},
        {"source_timebase_mode", "exact_rational"},
        {"rounding", "round_half_to_even"}}},
      {"canonical_analysis_raster",
       canonical_raster_to_json(plan.canonical_raster, plan.primary_video_stream)},
      {"probe", media_probe_to_json(plan.probe)},
      {"sample_frame_timestamps", frame_samples_to_json(plan.primary_video_stream)},
      {"media_ingest_outputs",
       {{"source_media_copy", "not_written"},
        {"analysis_audio", "not_written"},
        {"partial_package", false},
        {"package_writer", "out_of_scope_phase_06_foundation"}}},
  };
}

}  // namespace svp::media
