#include "svp/vision/foundation_color_staging.hpp"

#include "svp/vision/foundation_color_observations.hpp"
#include "svp/vision/real_frame_color_sampling.hpp"

#include <utility>

namespace svp::vision {
namespace {

ColorRasterFrame make_frame(std::string frame_id,
                            const std::int64_t timestamp_us,
                            const bool keyframe,
                            std::vector<Srgb8Pixel> pixels) {
  return ColorRasterFrame{
      std::move(frame_id),
      timestamp_us,
      2,
      2,
      keyframe,
      std::move(pixels),
  };
}

ColorTimelineRange make_range(std::string target_id,
                              const std::int64_t start_us,
                              const std::int64_t end_us,
                              std::vector<std::string> frame_ids) {
  return ColorTimelineRange{
      std::move(target_id),
      start_us,
      end_us,
      std::move(frame_ids),
  };
}

nlohmann::json make_processor_provenance(const std::string& provenance_id) {
  return {
      {"id", provenance_id},
      {"processor_type", "color_bucket_quantizer"},
      {"processor_version", "svp-vision-foundation-color-staging-v1"},
      {"runtime", "deterministic_cpp"},
      {"execution_provider", "cpu"},
      {"model_refs", nlohmann::json::array()},
      {"color_space", registered_color_space()},
      {"color_bucket_registry_version", registered_color_bucket_version()},
      {"quantization_method", "deterministic_srgb8_registered_bucket_quantization"},
      {"sampling_basis", {"full_frame", "keyframe_full_frame"}},
      {"canonical_raster_basis", "synthetic_in_memory_2x2_frames"},
      {"rounding_and_precision_rules",
       "coverage ratios are exact sample-count fractions serialized as JSON numbers"},
      {"target_types", {"frame", "scene", "shot"}},
      {"quality_metrics",
       {{"quality_score", "1.0 for non-empty deterministic foundation samples"}}},
  };
}

nlohmann::json make_manifest(const ColorObservationRecordPlan& records,
                             const std::string& provenance_id) {
  return {
      {"schema_version", "svp-builder-foundation-color-staging-v1"},
      {"execution_state", "foundation_synthetic_sample_only"},
      {"input_kind", "synthetic_in_memory_color_frames"},
      {"real_media_frame_decoding_run", false},
      {"canonical_raster_frame_source",
       "deterministic in-memory 2x2 sample frames"},
      {"package_writer_run", false},
      {"valid_svp_package_written", false},
      {"color_observation_count", records.records.size()},
      {"color_summary_written", true},
      {"color_absence_written", true},
      {"processor_provenance_written", true},
      {"provenance_id", provenance_id},
      {"notes",
       {"This is a builder staging artifact for the foundation color pipeline.",
        "It proves RC2-shaped color records can be produced from deterministic sample inputs.",
        "It does not claim observations were decoded from the requested media source.",
        "It does not claim a final .svp package was written."}},
  };
}

// Provenance for the real-frame path.  canonical_raster_basis reflects the
// actual decoding target so downstream readers can distinguish the two paths.
nlohmann::json make_real_processor_provenance(
    const std::string& provenance_id,
    const media::MediaIngestPlan& plan,
    bool decoding_succeeded) {
  const std::string raster_basis = decoding_succeeded
      ? ("ffmpeg_decoded_canonical_raster_" +
         std::to_string(plan.canonical_raster.width) + "x" +
         std::to_string(plan.canonical_raster.height))
      : "synthetic_fallback_2x2_frames";

  return {
      {"id", provenance_id},
      {"processor_type", "color_bucket_quantizer"},
      {"processor_version", "svp-vision-real-frame-color-staging-v1"},
      {"runtime", "deterministic_cpp"},
      {"execution_provider", "cpu"},
      {"model_refs", nlohmann::json::array()},
      {"color_space", registered_color_space()},
      {"color_bucket_registry_version", registered_color_bucket_version()},
      {"quantization_method", "deterministic_srgb8_registered_bucket_quantization"},
      {"sampling_basis", {"full_frame", "keyframe_full_frame"}},
      {"canonical_raster_basis", raster_basis},
      {"rounding_and_precision_rules",
       "coverage ratios are exact sample-count fractions serialized as JSON numbers"},
      {"target_types", {"frame", "scene", "shot"}},
      {"quality_metrics",
       {{"quality_score", "1.0 for non-empty deterministic foundation samples"}}},
  };
}

nlohmann::json make_real_manifest(const ColorObservationRecordPlan& records,
                                  const std::string& provenance_id,
                                  const media::MediaIngestPlan& plan,
                                  const RealFrameSamplingResult& sampling_result) {
  // Four honest execution states, mutually exclusive with the flag triple:
  //   real_decoding_attempted / real_media_frame_decoding_run
  //
  //  ffmpeg_unavailable_synthetic_fallback
  //      attempted=false, decoding_run=false — ffmpeg not found or
  //      pre-conditions not met; synthetic fallback used, source not touched.
  //
  //  real_frame_decoding_all_missed_synthetic_fallback
  //      attempted=true,  decoding_run=false — ffmpeg ran but every targeted
  //      timestamp returned 0 bytes; synthetic fallback used.
  //
  //  real_frame_decoding_partial
  //      attempted=true,  decoding_run=true  — some timestamps decoded,
  //      some missed; observations come from the decoded subset.
  //
  //  real_frame_decoding_succeeded
  //      attempted=true,  decoding_run=true  — all targeted timestamps
  //      decoded successfully; no synthetic data used.

  std::string execution_state;
  if (!sampling_result.real_decoding_attempted) {
    execution_state = "ffmpeg_unavailable_synthetic_fallback";
  } else if (!sampling_result.real_decoding_succeeded) {
    execution_state = "real_frame_decoding_all_missed_synthetic_fallback";
  } else if (sampling_result.frames_missed > 0) {
    execution_state = "real_frame_decoding_partial";
  } else {
    execution_state = "real_frame_decoding_succeeded";
  }

  const std::string raster_source = sampling_result.real_decoding_succeeded
      ? ("ffmpeg decoded " + std::to_string(sampling_result.frames_decoded) +
         " frame(s) scaled to " +
         std::to_string(plan.canonical_raster.width) + "x" +
         std::to_string(plan.canonical_raster.height) +
         (sampling_result.frames_missed > 0
              ? " (" + std::to_string(sampling_result.frames_missed) + " missed)"
              : ""))
      : ("synthetic fallback (2x2): " + sampling_result.skipped_reason);

  return {
      {"schema_version", "svp-builder-foundation-color-staging-v1"},
      {"execution_state", execution_state},
      {"input_kind", sampling_result.real_decoding_succeeded
                         ? "real_decoded_canonical_raster_frames"
                         : "synthetic_in_memory_color_frames"},
      {"real_media_frame_decoding_run", sampling_result.real_decoding_succeeded},
      {"real_decoding_attempted", sampling_result.real_decoding_attempted},
      {"frames_attempted", sampling_result.frames_attempted},
      {"frames_decoded", sampling_result.frames_decoded},
      {"frames_missed", sampling_result.frames_missed},
      {"canonical_raster_frame_source", raster_source},
      {"source_path", plan.source_path.string()},
      {"canonical_raster_width", plan.canonical_raster.width},
      {"canonical_raster_height", plan.canonical_raster.height},
      {"package_writer_run", false},
      {"valid_svp_package_written", false},
      {"color_observation_count", records.records.size()},
      {"color_summary_written", true},
      {"color_absence_written", true},
      {"processor_provenance_written", true},
      {"provenance_id", provenance_id},
      {"notes",
       {"real_media_frame_decoding_run is true only when at least one frame was decoded.",
        "real_decoding_attempted is false when ffmpeg was not found or pre-conditions failed.",
        "frames_missed > 0 means some timestamps were skipped; observations use decoded subset.",
        "valid_svp_package_written is always false until Phase 10 package writing exists."}},
  };
}


}  // namespace

ColorFrameSamplingInput build_foundation_color_staging_sample_input() {
  return ColorFrameSamplingInput{
      {
          make_frame("frame_000001", 0, true,
                     {{255, 128, 0}, {255, 128, 0}, {255, 128, 0}, {0, 0, 0}}),
          make_frame("frame_000002", 500000, false,
                     {{255, 255, 255}, {255, 255, 255}, {0, 0, 0}, {0, 0, 0}}),
          make_frame("frame_000003", 1000000, true,
                     {{255, 160, 0}, {255, 160, 0}, {255, 160, 0}, {255, 160, 0}}),
      },
      {
          make_range("scene_000001", 0, 1000000,
                     {"frame_000001", "frame_000002", "frame_000003"}),
      },
      {
          make_range("shot_000001", 0, 500000, {"frame_000001", "frame_000002"}),
          make_range("shot_000002", 500000, 1000000,
                     {"frame_000002", "frame_000003"}),
      },
  };
}

FoundationColorStagingArtifact build_foundation_color_staging_artifact() {
  constexpr const char* provenance_id = "processor_color_quantizer_0001";
  FoundationColorStagingArtifact artifact;
  artifact.records = build_foundation_color_observations(
      build_foundation_color_staging_sample_input(),
      FoundationColorObservationOptions{provenance_id, 1});
  artifact.processor_provenance = make_processor_provenance(provenance_id);
  artifact.manifest = make_manifest(artifact.records, provenance_id);
  return artifact;
}

nlohmann::json foundation_color_staging_artifact_to_json(
    const FoundationColorStagingArtifact& artifact) {
  return {
      {"manifest", artifact.manifest},
      {"colors",
       {{"color_observations",
         color_observation_records_to_jsonl_array(artifact.records.records)},
        {"color_summary", artifact.records.color_summary},
        {"color_absence", artifact.records.color_absence}}},
      {"provenance", {{"processors", {artifact.processor_provenance}}}},
  };
}

FoundationColorStagingArtifact build_real_frame_color_staging_artifact(
    const media::MediaIngestPlan& plan,
    const std::filesystem::path& ffmpeg_path) {
  constexpr const char* provenance_id = "processor_color_quantizer_0001";

  const RealFrameSamplingResult sampling_result =
      build_real_frame_color_sampling_input(plan, ffmpeg_path);

  FoundationColorStagingArtifact artifact;
  artifact.records = build_foundation_color_observations(
      sampling_result.input,
      FoundationColorObservationOptions{provenance_id, 1});
  artifact.processor_provenance =
      make_real_processor_provenance(provenance_id, plan,
                                     sampling_result.real_decoding_succeeded);
  artifact.manifest =
      make_real_manifest(artifact.records, provenance_id, plan, sampling_result);
  return artifact;
}

}  // namespace svp::vision
