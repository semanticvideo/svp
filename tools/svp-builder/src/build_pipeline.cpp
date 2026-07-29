#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_progress.hpp"
#include "svp/builder/model_cache_preflight.hpp"

#include "build_pipeline_internal.hpp"
#include "staging_cleanup.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/whisper_model.hpp"
#include "svp/core/memory_diagnostics.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/cache.hpp"
#include "svp/models/runtime.hpp"
#include "svp/vision/noise_suppression.hpp"

#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace svp::builder {

namespace {

bool should_write_builder_foundation_json(
    const BuildPipelineOptions& options,
    const BuildStageExecutionPlan& stage_plan,
    const PackageSkeletonStageResult& package_result) {
  if (options.verbose) {
    return true;
  }
  if (!stage_plan.run_package_skeleton) {
    return true;
  }
  return package_result.json_output_path == options.output_path;
}

void remove_builder_foundation_json(
    const std::filesystem::path& json_output_path) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(json_output_path, ec)) {
    std::filesystem::remove(json_output_path, ec);
  }
}

}  // namespace

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

    BuildPipelineOptions effective_options = options;
    if (effective_options.model_cache_dir.empty()) {
      effective_options.model_cache_dir = svp::models::model_cache_root();
    }

    const std::string stop_after_name(build_stage_name(effective_options.stop_after));
    const BuildStageExecutionPlan stage_plan =
        execution_plan_for_stage(effective_options.stop_after);

    if (schedules_model_backed_work(stage_plan)) {
      verify_authoritative_model_cache(effective_options.model_cache_dir);
    }

    sink->emit(make_stage_started(ProgressStageId::media_probe));
    const svp::media::MediaIngestPlan plan =
        svp::media::build_media_ingest_plan(
            options.source_path,
            load_or_run_probe(options.source_path, options.probe_json_path,
                              options.ffprobe_path));
    nlohmann::json output = svp::media::media_ingest_plan_to_json(plan);
    sink->emit(make_stage_completed(ProgressStageId::media_probe));

    const bool user_supplied_staging = !effective_options.staging_dir.empty();
    const std::filesystem::path staging_dir =
        user_supplied_staging
            ? effective_options.staging_dir
            : default_staging_dir_for_output(effective_options.output_path);
    StagingCleanupGuard staging_guard(staging_dir, user_supplied_staging);
    const bool model_runtime_available = svp::models::OnnxSession::is_available();
    svp::models::set_onnx_verbose(options.verbose);
    svp::audio::set_whisper_verbose(options.verbose);
    svp::vision::set_opencv_verbose(options.verbose);

    if (!effective_options.sherpa_lib_path.empty()) {
      svp::audio::set_sherpa_lib_path(effective_options.sherpa_lib_path);
    }

    BuildPipelineContext context{effective_options, stage_plan, plan, staging_dir,
                                 model_runtime_available, output,
                                 svp::vision::FrameCatalog{}, *sink};

    PackageSkeletonStageResult package_result;
    package_result.json_output_path = options.output_path;

    if (stage_plan.run_package_skeleton) {
      if (stage_plan.run_foundation_color) {
        emit_stage_started(context, ProgressStageId::color);
        run_foundation_color_stage(context);
        emit_stage_completed(context, ProgressStageId::color);
      }

      if (effective_options.serial_pipeline) {
        const PackageVisionStageResult vision_result =
            run_package_vision_stage(context);
        if (const std::optional<int> audio_exit = run_audio_stage(context)) {
          return {.exit_code = *audio_exit};
        }
        package_result = run_package_final_stage(context, vision_result);
      } else {
        auto run_audio_lane = [](BuildPipelineContext& audio_context) {
          return run_audio_stage(audio_context);
        };

        nlohmann::json audio_output = output;
        BuildPipelineContext audio_context{
            effective_options, stage_plan, plan, staging_dir,
            model_runtime_available, audio_output,
            svp::vision::FrameCatalog{}, *sink};

        const BuilderConcurrencyPolicy policy =
            builder_concurrency_policy(effective_options.performance, 1);
        PackageVisionStageResult vision_result;
        std::optional<int> audio_exit;
        if (policy.single_video_heavy_lanes > 1) {
          auto audio_future =
              std::async(std::launch::async, [&run_audio_lane, &audio_context]() {
                return run_audio_lane(audio_context);
              });
          vision_result = run_package_vision_stage(context);
          audio_exit = audio_future.get();
        } else {
          audio_exit = run_audio_lane(audio_context);
          if (!audio_exit) {
            vision_result = run_package_vision_stage(context);
          }
        }
        if (audio_output.contains("audio_foundation")) {
          output["audio_foundation"] = audio_output["audio_foundation"];
        }
        if (audio_exit) {
          return {.exit_code = *audio_exit};
        }

        package_result = run_package_final_stage(context, vision_result);
      }
    } else if (stage_plan.run_audio) {
      if (const std::optional<int> audio_exit = run_audio_stage(context)) {
        return {.exit_code = *audio_exit};
      }
    }
    if (!stage_plan.run_package_skeleton && stage_plan.run_vision_plan) {
      emit_stage_started(context, ProgressStageId::vision_plan);
      run_vision_plan_stage(context);
      emit_stage_completed(context, ProgressStageId::vision_plan);
    }
    if (!stage_plan.run_package_skeleton && stage_plan.run_foundation_color) {
      emit_stage_started(context, ProgressStageId::color);
      run_foundation_color_stage(context);
      emit_stage_completed(context, ProgressStageId::color);
    }
    if (!stage_plan.run_package_skeleton && stage_plan.run_foundation_ocr) {
      emit_stage_started(context, ProgressStageId::ocr);
      run_foundation_ocr_stage(context);
      emit_stage_completed(context, ProgressStageId::ocr);
    }

    output["builder_command"] = {
        {"command", "build"},
        {"stop_after", stop_after_name},
        {"ocr_performance", effective_options.performance.ocr_performance_profile},
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

    const bool write_foundation_json =
        should_write_builder_foundation_json(options, stage_plan, package_result);
    if (write_foundation_json) {
      write_json_file(package_result.json_output_path, output);
      emit_artifact_written(context,
                            stage_plan.run_package_skeleton
                                ? ProgressStageId::package_write
                                : ProgressStageId::media_probe,
                            package_result.json_output_path,
                            "builder foundation JSON");
    } else {
      remove_builder_foundation_json(package_result.json_output_path);
    }

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
