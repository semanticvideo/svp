#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_progress.hpp"

#include "build_pipeline_internal.hpp"
#include "staging_cleanup.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/whisper_model.hpp"
#include "svp/core/memory_diagnostics.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/runtime.hpp"
#include "svp/vision/noise_suppression.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace svp::builder {

BuildPipelineResult BuildPipeline::run(const BuildPipelineOptions& options) const {
  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

  try {
    std::ostringstream diag_name;
    diag_name << "svp-builder-memory";
#if defined(__APPLE__)
    diag_name << "-" << static_cast<long long>(getpid());
#endif
    diag_name << ".jsonl";
    svp::core::configure_memory_diagnostics_from_environment(
        std::filesystem::current_path() / "build" / "diagnostics" /
        diag_name.str());
    svp::core::check_memory_limit("builder.run.start", {
        {"source", options.source_path},
        {"output", options.output_path.string()},
        {"staging_dir", options.staging_dir.string()}
    });

    const std::string stop_after_name(build_stage_name(options.stop_after));
    const BuildStageExecutionPlan stage_plan =
        execution_plan_for_stage(options.stop_after);

    sink->emit(make_stage_started(ProgressStageId::media_probe));
    const svp::media::MediaIngestPlan plan =
        svp::media::build_media_ingest_plan(
            options.source_path,
            load_or_run_probe(options.source_path, options.probe_json_path,
                              options.ffprobe_path));
    nlohmann::json output = svp::media::media_ingest_plan_to_json(plan);
    sink->emit(make_stage_completed(ProgressStageId::media_probe));

    const bool user_supplied_staging = !options.staging_dir.empty();
    const std::filesystem::path staging_dir =
        user_supplied_staging
            ? options.staging_dir
            : default_staging_dir_for_output(options.output_path);
    StagingCleanupGuard staging_guard(staging_dir, user_supplied_staging);
    const bool model_runtime_available = svp::models::OnnxSession::is_available();
    svp::models::set_onnx_verbose(options.verbose);
    svp::audio::set_whisper_verbose(options.verbose);
    svp::vision::set_opencv_verbose(options.verbose);

    if (!options.sherpa_lib_path.empty()) {
      svp::audio::set_sherpa_lib_path(options.sherpa_lib_path);
    }

    BuildPipelineContext context{options, stage_plan, plan, staging_dir,
                                 model_runtime_available, output,
                                 svp::vision::FrameCatalog{}, *sink};

    if (stage_plan.run_audio) {
      emit_stage_started(context, ProgressStageId::audio_extract);
      if (const std::optional<int> audio_exit = run_audio_stage(context)) {
        emit_stage_failed(context, ProgressStageId::audio_extract);
        return {.exit_code = *audio_exit};
      }
      emit_stage_completed(context, ProgressStageId::audio_extract);
    }
    if (stage_plan.run_vision_plan) {
      emit_stage_started(context, ProgressStageId::vision_plan);
      run_vision_plan_stage(context);
      emit_stage_completed(context, ProgressStageId::vision_plan);
    }
    if (stage_plan.run_foundation_color) {
      emit_stage_started(context, ProgressStageId::color);
      run_foundation_color_stage(context);
      emit_stage_completed(context, ProgressStageId::color);
    }
    if (stage_plan.run_foundation_ocr) {
      emit_stage_started(context, ProgressStageId::ocr);
      run_foundation_ocr_stage(context);
      emit_stage_completed(context, ProgressStageId::ocr);
    }

    PackageSkeletonStageResult package_result;
    package_result.json_output_path = options.output_path;
    if (stage_plan.run_package_skeleton) {
      package_result = run_package_skeleton_stage(context);
    }

    output["builder_command"] = {
        {"command", "build"},
        {"stop_after", stop_after_name},
        {"ocr_performance", options.performance.ocr_performance_profile},
        {"valid_svp_package_written", package_result.validator_passes},
    };

    if (stage_plan.run_package_skeleton) {
      output["builder_command"]["package_path"] =
          package_result.package_path.string();
      output["builder_command"]["validator"] = {
          {"exit_code", package_result.validator_exit_code},
          {"validation_report_stored", package_result.validation_report_stored},
          {"validator_proven_valid", package_result.validator_passes},
          {"report", package_result.validation_report_json}};
      if (package_result.validation_report_stored) {
        output["builder_command"]["validator"]["validation_report_path"] =
            "provenance/validation.json";
      }
    }

    write_json_file(package_result.json_output_path, output);
    emit_artifact_written(context,
                          stage_plan.run_package_skeleton
                              ? ProgressStageId::package_write
                              : ProgressStageId::media_probe,
                          package_result.json_output_path,
                          "builder foundation JSON");

    if (options.verbose) {
      print_build_progress(context, package_result);
    }

    if (stage_plan.run_package_skeleton && package_result.package_written &&
        !package_result.validator_passes) {
      svp::core::check_memory_limit("builder.run.complete.validator_failed");
      return {.exit_code = package_result.validator_exit_code};
    }
    svp::core::check_memory_limit("builder.run.complete");
    staging_guard.cleanup_on_success();
    return {.exit_code = 0};
  } catch (const std::exception& error) {
    svp::core::trace_memory_event("builder.run.exception", {
        {"error", error.what()}
    });
    std::cerr << "svp-builder: " << error.what() << "\n";
    return {.exit_code = 1};
  }
}

}  // namespace svp::builder
