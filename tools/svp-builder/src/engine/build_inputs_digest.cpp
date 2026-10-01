#include "engine/build_inputs_digest.hpp"

#include "svp/core/version.hpp"
#include "svp/exec/blake3_digest.hpp"

namespace svp::builder::engine {

nlohmann::json describe_build_inputs(const BuildPipelineOptions& options,
                                     const svp::exec::SourceFingerprintRecord& source,
                                     const nlohmann::json& ingest_plan_json,
                                     const svp::models::ThreadPlan& thread_plan) {
  nlohmann::json description = {
      {"builder_version", std::string(svp::core::kToolVersion)},
      {"source",
       {{"path", options.source_path},
        {"size_bytes", source.size_bytes},
        {"blake3", svp::exec::blake3_hex(source.blake3)}}},
      {"ingest_plan", ingest_plan_json},
      {"options",
       {{"stop_after", std::string(build_stage_name(options.stop_after))},
        {"ocr_performance_profile", options.performance.ocr_performance_profile},
        {"visual_tracking_quality", options.visual_tracking_quality},
        {"force_single_speaker", options.force_single_speaker},
        {"allow_fallback_diarization", options.allow_fallback_diarization},
        {"serial_pipeline", options.serial_pipeline},
        {"model_cache_dir", options.model_cache_dir.string()}}},
      {"runtime_tools",
       {{"ffmpeg", options.ffmpeg_path},
        {"ffprobe", options.ffprobe_path},
        {"sherpa_lib", options.sherpa_lib_path}}},
      {"thread_plan", svp::models::thread_plan_to_json(thread_plan)},
  };
  // The published artifact. interlace create builds its package at a
  // temporary staging path, which therefore stays out of the digest.
  if (!options.svpi) {
    description["output_path"] = options.output_path.string();
  } else {
    description["svpi"] = {
        {"path", options.svpi->svpi_path.string()},
        {"compute_full_blake3", options.svpi->compute_full_blake3},
        {"compute_chunk_proof", options.svpi->compute_chunk_proof}};
  }
  return description;
}

std::string build_inputs_blake3(const nlohmann::json& description) {
  return svp::exec::blake3_hex(svp::exec::blake3_digest(description.dump()));
}

}  // namespace svp::builder::engine
