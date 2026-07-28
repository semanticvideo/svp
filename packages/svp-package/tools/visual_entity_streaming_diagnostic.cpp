#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"
#include "svp/package/visual_entity_artifact_writer.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"

#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: svp-visual-entity-streaming-diagnostic "
                 "<media> <model-cache> <ffmpeg> <ffprobe> <staging-dir>\n";
    return 2;
  }
  try {
    const std::filesystem::path media_path = argv[1];
    const std::filesystem::path model_cache = argv[2];
    const std::filesystem::path ffmpeg = argv[3];
    const std::filesystem::path ffprobe = argv[4];
    const std::filesystem::path staging_dir = argv[5];
    if (std::filesystem::exists(staging_dir) &&
        !std::filesystem::is_empty(staging_dir)) {
      throw std::invalid_argument("staging directory must be empty");
    }

    auto probe = svp::media::probe_media_with_ffprobe(media_path, ffprobe);
    auto plan =
        svp::media::build_media_ingest_plan(media_path, std::move(probe));
    svp::package::VisualEntityArtifactWriter writer(staging_dir);
    svp::vision::VisualEntityPipelineOptions options;
    options.assembly.handoff_retention_us = options.sampling.window_overlap_us;
    options.assembly.artifact_sink =
        [&writer](const std::vector<svp::vision::TrackedRegion>& regions,
                  const std::vector<svp::vision::MaskWriteEntry>& masks) {
          writer.append(regions, masks);
        };
    const auto result = svp::vision::run_visual_entity_pipeline(
        plan, ffmpeg, model_cache, {}, nullptr, options);

    std::set<std::string> retained_entity_ids;
    for (const auto& entity : result.assembled.tracker_result.entities) {
      retained_entity_ids.insert(entity.entity_id);
    }
    const auto artifacts = writer.finish(retained_entity_ids);
    const nlohmann::json output = {
        {"windows_planned", result.windows_planned},
        {"windows_processed", result.windows_processed},
        {"windows_succeeded", result.windows_succeeded},
        {"frames_attempted", result.frames_attempted},
        {"frames_decoded", result.frames_decoded},
        {"frames_missed", result.frames_missed},
        {"entity_count", result.assembled.tracker_result.entities.size()},
        {"track_count", result.assembled.tracker_result.tracks.size()},
        {"region_count", artifacts.region_count},
        {"mask_count", artifacts.mask_count},
        {"processor_status", result.assembled.tracker_result.processing_status},
        {"failures", result.failures},
        {"blocker", result.blocker},
        {"staging_dir", staging_dir.string()}};
    std::cout << output.dump(2) << '\n';
    return result.blocker.empty() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
