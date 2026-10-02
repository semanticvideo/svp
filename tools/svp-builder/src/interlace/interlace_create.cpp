#include "svp/builder/interlace.hpp"
#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_progress.hpp"

#include "default_staging.hpp"
#include "staging_cleanup.hpp"
#include "svpi_publication.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/svpi_writer.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/report_json.hpp"
#include "svp/package/output_directory.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace svp::builder {

InterlaceCreateResult interlace_create(const InterlaceCreateOptions& options) {
  InterlaceCreateResult result;
  result.svpi_path = options.output_path;

  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

  const std::filesystem::path source_path(options.source_path);
  if (!std::filesystem::exists(source_path)) {
    result.error_message = "source media file does not exist: " + options.source_path;
    return result;
  }
  const SvpiWriteRequest request{.source_path = options.source_path,
                                 .output_path = options.output_path};

  auto create_binding = [&]() {
    sink->emit(make_stage_started(ProgressStageId::media_binding));
    svp::package::MediaBindingFactoryOptions binding_opts;
    binding_opts.ffprobe_path = options.ffprobe_path;
    binding_opts.compute_full_blake3 = options.compute_full_blake3;
    binding_opts.compute_chunk_proof = options.compute_chunk_proof;
    auto binding =
        svp::package::create_media_binding(source_path, binding_opts);
    sink->emit(make_stage_completed(ProgressStageId::media_binding));
    return binding;
  };

  const bool user_supplied_staging = !options.staging_dir.empty();
  const std::filesystem::path staging_dir =
      user_supplied_staging
          ? std::filesystem::path(options.staging_dir)
          : make_default_staging_dir();
  StagingCleanupGuard staging_guard(staging_dir, user_supplied_staging);

  if (options.core_only_diagnostic) {
    auto binding_doc = create_binding();
    result.blake3_state = svp::package::to_string(
        binding_doc.bindings[0].identity.blake3_state);
    result = write_core_only_svpi(
        request, binding_doc, result.blake3_state,
        "not_generated",
        "SVPI sidecar created in core-only diagnostic mode (semantic pipeline skipped)",
        staging_dir, *sink);
    if (result.success) staging_guard.cleanup_on_success();
    return result;
  }

  std::filesystem::path temp_svp_path =
      staging_dir / "interlace_temp.svp";

  BuildPipelineOptions pipeline_opts;
  pipeline_opts.source_path = options.source_path;
  pipeline_opts.probe_json_path = options.probe_json_path;
  pipeline_opts.ffprobe_path = options.ffprobe_path;
  pipeline_opts.ffmpeg_path = options.ffmpeg_path;
  pipeline_opts.output_path = temp_svp_path;
  pipeline_opts.staging_dir = staging_dir;
  pipeline_opts.model_cache_dir = options.model_cache_dir;
  pipeline_opts.stop_after = BuildStage::package_skeleton;
  pipeline_opts.performance = options.performance;
  pipeline_opts.visual_tracking_quality = options.visual_tracking_quality;
  pipeline_opts.sherpa_lib_path = options.sherpa_lib_path;
  pipeline_opts.allow_fallback_diarization = options.allow_fallback_diarization;
  pipeline_opts.force_single_speaker = options.force_single_speaker;
  pipeline_opts.serial_pipeline = options.serial_pipeline;
  pipeline_opts.distributed = options.distributed;
  pipeline_opts.thread_plan = options.thread_plan;
  pipeline_opts.reset_staging_before_stages = true;
  pipeline_opts.progress_sink = sink;
  // Binding and the SVPI write are tasks of the same journaled build, and the
  // journal belongs to the .svpi the user asked for.
  pipeline_opts.journal_mode = options.journal_mode;
  pipeline_opts.journal_output_path = options.output_path;
  pipeline_opts.svpi = SvpiPublicationOptions{
      .svpi_path = options.output_path,
      .compute_full_blake3 = options.compute_full_blake3,
      .compute_chunk_proof = options.compute_chunk_proof};

  BuildPipeline pipeline;
  auto pipeline_result = pipeline.run(pipeline_opts);
  result.thread_plan = pipeline_result.thread_plan;
  result.pipeline_failure = pipeline_result.failure;
  result.pipeline_exit_code = pipeline_result.exit_code;

  if (pipeline_result.svpi) {
    result.success = pipeline_result.svpi->success;
    result.error_message = pipeline_result.svpi->error_message;
    result.blake3_state = pipeline_result.svpi->blake3_state;
    result.binding_state = pipeline_result.svpi->binding_state;
    if (result.success) staging_guard.cleanup_on_success();
    return result;
  }

  // The build stopped before publishing. Model preflight, recovery-journal,
  // and cancellation failures publish nothing.
  if (pipeline_result.failure == BuildPipelineFailure::model_cache_preflight ||
      pipeline_result.failure == BuildPipelineFailure::recovery_journal ||
      pipeline_result.failure == BuildPipelineFailure::cancelled) {
    result.error_message = pipeline_result.error_message;
    return result;
  }

  // A stage failed: publish what staging holds, as before (core-only when the
  // semantic pipeline produced nothing). The recovery journal is kept so the
  // build can be resumed once the failure is fixed.
  auto binding_doc = create_binding();
  result.blake3_state = svp::package::to_string(
      binding_doc.bindings[0].identity.blake3_state);

  if (std::filesystem::exists(temp_svp_path)) {
    std::filesystem::remove(temp_svp_path);
  }
  if (std::filesystem::exists(temp_svp_path.string() + ".json")) {
    std::filesystem::remove(temp_svp_path.string() + ".json");
  }

  const nlohmann::json sections = detect_section_states(staging_dir);
  const bool semantic_content = has_semantic_content(sections);
  InterlaceCreateResult published;
  if (!semantic_content) {
    std::filesystem::remove_all(staging_dir);
    published = write_core_only_svpi(request, binding_doc, result.blake3_state,
                                     "blocked", kSvpiBlockedNotes, staging_dir, *sink);
  } else {
    published = write_svpi_from_staging(request, binding_doc, result.blake3_state,
                                        staging_dir, sections,
                                        svpi_provenance_notes(semantic_content), *sink);
  }
  published.thread_plan = result.thread_plan;
  published.pipeline_failure = result.pipeline_failure;
  published.pipeline_exit_code = result.pipeline_exit_code;
  if (published.success) staging_guard.cleanup_on_success();
  return published;
}

}  // namespace svp::builder
