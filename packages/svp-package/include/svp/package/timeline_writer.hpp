#pragma once

#include <filesystem>

namespace svp::media { struct MediaIngestPlan; }
namespace svp::vision { struct FoundationColorStagingArtifact; }

namespace svp::package {

struct TimelineWriteSummary {
  bool frames_written = false;
  bool shots_written = false;
  bool scenes_written = false;
  std::size_t frame_count = 0;
  std::size_t shot_count = 0;
  std::size_t scene_count = 0;
};

/**
 * Writes honest, source-derived timeline artifacts to staging:
 * - timeline/frames.jsonl
 * - timeline/shots.jsonl
 * - timeline/scenes.jsonl
 *
 * It uses the ColorFrameSamplingInput embedded in the FoundationColorStagingArtifact
 * to prevent double-decoding video frames.
 *
 * Also appends a processor record for the timeline generation to
 * provenance/processors.jsonl.
 */
[[nodiscard]] TimelineWriteSummary write_timeline_artifacts(
    const std::filesystem::path& staging_dir,
    const svp::media::MediaIngestPlan& plan,
    const svp::vision::FoundationColorStagingArtifact& color_artifact);

} // namespace svp::package
