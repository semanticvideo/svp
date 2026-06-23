#include "svp/builder/build_pipeline.hpp"

#include "build_pipeline_internal.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/runtime.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace svp::builder {

BuildPipelineResult BuildPipeline::run(const BuildPipelineOptions& options) const {
  try {
    const std::string stop_after_name(build_stage_name(options.stop_after));
    const BuildStageExecutionPlan stage_plan =
        execution_plan_for_stage(options.stop_after);
    const svp::media::MediaIngestPlan plan =
        svp::media::build_media_ingest_plan(
            options.source_path,
            load_or_run_probe(options.source_path, options.probe_json_path,
                              options.ffprobe_path));
    nlohmann::json output = svp::media::media_ingest_plan_to_json(plan);

    const std::filesystem::path staging_dir =
        options.staging_dir.empty()
            ? default_staging_dir_for_output(options.output_path)
            : options.staging_dir;
    const bool model_runtime_available = svp::models::OnnxSession::is_available();

    if (!options.sherpa_lib_path.empty()) {
      svp::audio::set_sherpa_lib_path(options.sherpa_lib_path);
    }

    BuildPipelineContext context{options, stage_plan, plan, staging_dir,
                                 model_runtime_available, output};

    if (stage_plan.run_audio) {
      if (const std::optional<int> audio_exit = run_audio_stage(context)) {
        return {.exit_code = *audio_exit};
      }
    }
    if (stage_plan.run_vision_plan) {
      run_vision_plan_stage(context);
    }
    if (stage_plan.run_foundation_color) {
      run_foundation_color_stage(context);
    }
    if (stage_plan.run_foundation_ocr) {
      run_foundation_ocr_stage(context);
    }

    PackageSkeletonStageResult package_result;
    package_result.json_output_path = options.output_path;
    if (stage_plan.run_package_skeleton) {
      package_result = run_package_skeleton_stage(context);
    }

    output["builder_command"] = {
        {"command", "build"},
        {"stop_after", stop_after_name},
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
    std::cout << "Wrote builder foundation JSON: "
              << package_result.json_output_path << "\n";
    print_build_progress(context, package_result);

    if (stage_plan.run_package_skeleton && package_result.package_written &&
        !package_result.validator_passes) {
      return {.exit_code = package_result.validator_exit_code};
    }
    return {.exit_code = 0};
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return {.exit_code = 1};
  }
}

}  // namespace svp::builder
