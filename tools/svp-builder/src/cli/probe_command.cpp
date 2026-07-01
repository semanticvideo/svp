#include "cli_context.hpp"

#include "svp/media/media_ingest_plan.hpp"

#include <iostream>
#include <string>

namespace {

void print_plan_summary(const svp::media::MediaIngestPlan& plan) {
  std::cout << "SVP builder media ingest foundation\n";
  std::cout << "Source: " << plan.source_path.string() << "\n";
  std::cout << "Primary video stream: " << plan.primary_video_stream.id << "\n";
  std::cout << "Stored dimensions: " << plan.primary_video_stream.width << "x"
            << plan.primary_video_stream.height << "\n";
  std::cout << "Display aspect ratio: "
            << plan.canonical_raster.display.display_aspect_ratio << "\n";
  std::cout << "Canonical analysis raster: " << plan.canonical_raster.width << "x"
            << plan.canonical_raster.height << "\n";
  std::cout << "Audio streams: " << plan.probe.audio_streams.size() << "\n";
  std::cout << "Package writer: not run for this foundation command\n";
}

svp::media::MediaProbe load_or_run_probe(const std::string& source_path,
                                         const std::string& probe_json_path,
                                         const std::string& ffprobe_path) {
  if (!probe_json_path.empty()) {
    return svp::media::load_media_probe_json(probe_json_path);
  }
  return svp::media::probe_media_with_ffprobe(source_path, ffprobe_path);
}

}  // namespace

int run_probe_command(const ProbeCliOptions& options) {
  const svp::media::MediaIngestPlan plan =
      svp::media::build_media_ingest_plan(
          options.source_path,
          load_or_run_probe(options.source_path, options.probe_json_path,
                            options.ffprobe_path));
  if (options.json_output) {
    std::cout << svp::media::media_ingest_plan_to_json(plan).dump(2) << "\n";
  } else {
    print_plan_summary(plan);
  }
  return 0;
}
