#include "build_pipeline_internal.hpp"

#include "svp/core/memory_diagnostics.hpp"

#include <iostream>
#include <string>

namespace svp::builder {

void emit_progress(BuildPipelineContext& context, const ProgressEvent& event) {
  context.progress_sink.emit(event);
}

void emit_stage_started(BuildPipelineContext& context, ProgressStageId stage,
                        std::string message) {
  svp::core::check_memory_limit("stage.start", {
      {"stage", std::string(progress_stage_id(stage))},
      {"label", std::string(progress_stage_label(stage))},
      {"message", message}
  });
  emit_progress(context, make_stage_started(stage, std::move(message)));
}

void emit_stage_completed(BuildPipelineContext& context, ProgressStageId stage,
                          std::string message) {
  svp::core::check_memory_limit("stage.complete", {
      {"stage", std::string(progress_stage_id(stage))},
      {"label", std::string(progress_stage_label(stage))},
      {"message", message}
  });
  emit_progress(context, make_stage_completed(stage, std::move(message)));
}

void emit_stage_failed(BuildPipelineContext& context, ProgressStageId stage,
                       std::string message) {
  svp::core::check_memory_limit("stage.failed", {
      {"stage", std::string(progress_stage_id(stage))},
      {"label", std::string(progress_stage_label(stage))},
      {"message", message}
  });
  emit_progress(context, make_stage_failed(stage, std::move(message)));
}

void emit_warning(BuildPipelineContext& context, ProgressStageId stage,
                  std::string message) {
  emit_progress(context, make_warning(stage, std::move(message)));
}

void emit_artifact_written(BuildPipelineContext& context, ProgressStageId stage,
                           std::filesystem::path artifact_path,
                           std::string message) {
  emit_progress(context,
                make_artifact_written(stage, std::move(artifact_path),
                                      std::move(message)));
}

void emit_stage_progress(BuildPipelineContext& context, ProgressStageId stage,
                         std::uint64_t current, std::uint64_t total,
                         std::string unit, std::string message) {
  const bool should_sample =
      current == 0 || current == total || (current % 10) == 0;
  if (should_sample) {
    svp::core::check_memory_limit("stage.progress", {
        {"stage", std::string(progress_stage_id(stage))},
        {"label", std::string(progress_stage_label(stage))},
        {"current", std::to_string(current)},
        {"total", std::to_string(total)},
        {"unit", unit},
        {"message", message}
    });
  }
  emit_progress(context, make_stage_progress(stage, current, total,
                                             std::move(unit),
                                             std::move(message)));
}

void print_build_progress(const BuildPipelineContext& context,
                          const PackageSkeletonStageResult& package_result) {
  if (context.options.stop_after == BuildStage::audio) {
    std::cout << "Audio task plan only; no transcription or diarization was generated.\n";
    if (context.output.contains("audio_foundation") &&
        context.output["audio_foundation"].contains("asr_execution_boundary")) {
      const auto& asr = context.output["audio_foundation"]["asr_execution_boundary"];
      std::cout << "ASR status: " << asr["asr_status"] << "\n";
      std::cout << "ASR chunk count: " << asr["chunk_plan"]["chunk_count"] << "\n";
      std::cout << "ASR model available: " << asr["model_available"] << "\n";
      if (context.output["audio_foundation"].contains("transcript_write_result")) {
        const auto& twr = context.output["audio_foundation"]["transcript_write_result"];
        std::cout << "Transcript written: " << twr["transcript_written"] << "\n";
        std::cout << "Words written: " << twr["word_count"] << " words\n";
        std::cout << "Speakers written: " << twr["speaker_count"] << " speakers\n";
        if (context.output["audio_foundation"].contains("diarization_execution_boundary")) {
          const auto& deb = context.output["audio_foundation"]["diarization_execution_boundary"];
          if (deb["diarization_status"] == "fallback_one_speaker") {
            std::cout << "  WARNING: Diarization did NOT run. Speaker count is UNKNOWN.\n"
                      << "  Package speaker data is FABRICATED FALLBACK, not real.\n";
          }
        }
      }
    }
  }
  if (context.stage_plan.run_vision_plan) {
    std::cout << "Vision/OCR/color task plan only; no observations were generated.\n";
  }
  if (context.options.stop_after == BuildStage::foundation_color) {
    const bool real_run =
        context.output.at("foundation_color_staging").at("manifest").value(
            "real_media_frame_decoding_run", false);
    std::cout << "Staged foundation color observations under: "
              << context.staging_dir << "\n";
    if (real_run) {
      std::cout << "Real media frames were decoded for the color staging artifact.\n";
    } else {
      std::cout << "No real media frames were decoded; synthetic fallback was used.\n";
    }
  }
  if (context.stage_plan.run_foundation_ocr) {
    const bool ocr_detection_run =
        context.output.at("foundation_ocr_staging").value("ocr_detection_run", false);
    const bool ocr_available =
        context.output.at("foundation_ocr_staging").value("ocr_available", false);
    std::cout << "Staged foundation OCR observations under: "
              << context.staging_dir << "\n";
    std::cout << "OCR available: " << ocr_available << "\n";
    std::cout << "OCR detection run: " << ocr_detection_run << "\n";
    std::cout << "Text observation count: "
              << context.output.at("foundation_ocr_staging").value("text_observation_count", 0)
              << "\n";
    if (ocr_detection_run) {
      std::cout << "Real media frames were processed for OCR via PP-OCR ONNX.\n";
    } else {
      std::cout << "No real OCR was executed; honest absence was reported.\n";
      if (context.output.at("foundation_ocr_staging").contains("blocker") &&
          !context.output.at("foundation_ocr_staging").at("blocker").get<std::string>().empty()) {
        std::cout << "OCR blocker: "
                  << context.output.at("foundation_ocr_staging").at("blocker") << "\n";
      }
    }
  }
  if (context.stage_plan.run_package_skeleton) {
    std::cout << "Staged foundation files under: " << context.staging_dir << "\n";
    std::cout << "Relationships written: "
              << context.output.at("package_relationships_provenance").at("relationships_written")
              << "\n";
    std::cout << "Entities written: "
              << context.output.at("entity_artifacts").at("entity_count")
              << " entities, "
              << context.output.at("entity_artifacts").at("track_count")
              << " tracks\n";
    std::cout << "Processor provenance records written: "
              << context.output.at("package_relationships_provenance").at("processors_written")
              << "\n";
    std::cout << "Spatial/embedding placeholders written: "
              << context.output.at("spatial_embedding_placeholders").at("depth_index_written")
              << " depth index, "
              << context.output.at("spatial_embedding_placeholders").at("depth_blocks_written")
              << " depth blocks, "
              << context.output.at("spatial_embedding_placeholders").value("depth_placeholder_written", false)
              << " depth placeholder, "
              << context.output.at("spatial_embedding_placeholders").at("masks_blocks_written")
              << " masks blocks, "
              << context.output.at("spatial_embedding_placeholders").at("embedding_sets_written")
              << " embedding sets, "
              << context.output.at("spatial_embedding_placeholders").at("embeddings_blocks_written")
              << " embeddings blocks\n";
    std::cout << "Depth generation run: "
              << context.output.at("spatial_embedding_placeholders").at("depth_generation_run")
              << "\n";
    std::cout << "Embedding generation run: "
              << context.output.at("spatial_embedding_placeholders").at("embedding_generation_run")
              << "\n";
    std::cout << "Model runtime available: "
              << context.output.at("spatial_embedding_placeholders").at("model_runtime_available")
              << "\n";
    std::cout << "Depth model available: "
              << context.output.at("spatial_embedding_placeholders").at("depth_model_available")
              << "\n";
    std::cout << "Depth model file hashes verified: "
              << context.output.at("spatial_embedding_placeholders").value("depth_model_verified", false)
              << " (manifest file BLAKE3 hashes only; bundle_blake3 not yet verifiable)"
              << "\n";
    std::cout << "Depth frame input available: "
              << context.output.at("spatial_embedding_placeholders").value("depth_frame_input_available", false)
              << "\n";
    std::cout << "Embedding model available: "
              << context.output.at("spatial_embedding_placeholders").at("embedding_model_available")
              << "\n";
    std::cout << "OCR available: "
              << context.output.at("spatial_embedding_placeholders").value("ocr_available", false)
              << "\n";
    std::cout << "OCR frame input available: "
              << context.output.at("spatial_embedding_placeholders").value("ocr_frame_input_available", false)
              << "\n";
    std::cout << "OCR detection run: "
              << context.output.at("spatial_embedding_placeholders").value("ocr_detection_run", false)
              << "\n";
    std::cout << "OCR recognition run: "
              << context.output.at("spatial_embedding_placeholders").value("ocr_recognition_run", false)
              << "\n";
    std::cout << "Text observation count: "
              << context.output.at("spatial_embedding_placeholders").value("text_observation_count", 0)
              << "\n";
    std::cout << "Numeric value count: "
              << context.output.at("spatial_embedding_placeholders").value("numeric_value_count", 0)
              << "\n";
    if (context.output.at("spatial_embedding_placeholders").contains("depth_generation_detail") &&
        context.output.at("spatial_embedding_placeholders").at("depth_generation_detail").contains("blocker") &&
        !context.output.at("spatial_embedding_placeholders").at("depth_generation_detail").at("blocker").get<std::string>().empty()) {
      std::cout << "Depth blocker: "
                << context.output.at("spatial_embedding_placeholders").at("depth_generation_detail").at("blocker")
                << "\n";
    }
    if (context.output.at("spatial_embedding_placeholders").contains("embedding_generation_detail") &&
        context.output.at("spatial_embedding_placeholders").at("embedding_generation_detail").contains("blocker") &&
        !context.output.at("spatial_embedding_placeholders").at("embedding_generation_detail").at("blocker").get<std::string>().empty()) {
      std::cout << "Embedding blocker: "
                << context.output.at("spatial_embedding_placeholders").at("embedding_generation_detail").at("blocker")
                << "\n";
    }
    std::cout << "Wrote skeleton .svp package to: " << package_result.package_path << "\n";
    if (package_result.validation_report_stored) {
      std::cout << "Validation report stored at: provenance/validation.json\n";
    } else {
      std::cout << "Validation report storage FAILED\n";
    }
    std::cout << "Validator exit code: " << package_result.validator_exit_code << "\n";
    if (package_result.validator_passes) {
      std::cout << "Package validation: SUCCESS\n";
    } else {
      std::cout << "Package validation: INCOMPLETE/INVALID (Expected for skeleton package)\n";
    }
  } else {
    std::cout << "No .svp package was created by this foundation command.\n";
}}

}  // namespace svp::builder
