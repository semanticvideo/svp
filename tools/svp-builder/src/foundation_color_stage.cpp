#include "build_pipeline_internal.hpp"

#include "svp/package/timeline_writer.hpp"
#include "svp/vision/foundation_color_staging.hpp"

namespace {

void write_foundation_color_staging_files(
    const std::filesystem::path& staging_dir,
    const svp::vision::FoundationColorStagingArtifact& artifact) {
  const nlohmann::json color_observations =
      svp::vision::color_observation_records_to_jsonl_array(
          artifact.records.records);
  svp::builder::write_jsonl_file(staging_dir / "colors" / "color_observations.jsonl",
                                 color_observations);
  svp::builder::write_json_file(staging_dir / "colors" / "color_summary.json",
                                artifact.records.color_summary);
  svp::builder::write_json_file(staging_dir / "colors" / "color_absence.json",
                                artifact.records.color_absence);
  svp::builder::append_jsonl_file(staging_dir / "provenance" / "processors.jsonl",
                                  nlohmann::json::array({artifact.processor_provenance}));
}

}  // namespace

namespace svp::builder {

void run_foundation_color_stage(BuildPipelineContext& context) {
  svp::vision::FoundationColorStagingArtifact color_artifact =
      svp::vision::build_real_frame_color_staging_artifact(
          context.plan, context.options.ffmpeg_path, &context.frame_catalog);
  write_foundation_color_staging_files(context.staging_dir, color_artifact);

  const svp::package::TimelineWriteSummary timeline_summary =
      svp::package::write_timeline_artifacts(context.staging_dir, context.plan, color_artifact);

  nlohmann::json color_json =
      svp::vision::foundation_color_staging_artifact_to_json(color_artifact);

  for (auto& frame : color_artifact.sampling_input.frames) {
    frame.pixels.clear();
    frame.pixels.shrink_to_fit();
  }

  color_json["staging_paths"] = {
      {"color_observations_jsonl",
       (context.staging_dir / "colors" / "color_observations.jsonl").string()},
      {"color_summary_json",
       (context.staging_dir / "colors" / "color_summary.json").string()},
      {"color_absence_json",
       (context.staging_dir / "colors" / "color_absence.json").string()},
      {"processors_jsonl",
       (context.staging_dir / "provenance" / "processors.jsonl").string()},
      {"frames_jsonl",
       (context.staging_dir / "timeline" / "frames.jsonl").string()},
      {"shots_jsonl",
       (context.staging_dir / "timeline" / "shots.jsonl").string()},
      {"scenes_jsonl",
       (context.staging_dir / "timeline" / "scenes.jsonl").string()},
  };
  color_json["timeline_summary"] = {
      {"frames_written", timeline_summary.frames_written},
      {"shots_written", timeline_summary.shots_written},
      {"scenes_written", timeline_summary.scenes_written},
      {"frame_count", timeline_summary.frame_count},
      {"shot_count", timeline_summary.shot_count},
      {"scene_count", timeline_summary.scene_count}
  };
  context.output["foundation_color_staging"] = color_json;
}

}  // namespace svp::builder
