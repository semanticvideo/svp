#include "build_pipeline_internal.hpp"

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/audio_extraction_executor.hpp"
#include "svp/audio/audio_stage_plan.hpp"
#include "svp/audio/diarization_boundary.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/transcript_writer.hpp"
#include "svp/audio/vad_execution_boundary.hpp"
#include "svp/media/canonical_timing.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace svp::builder {

std::optional<int> run_audio_stage(BuildPipelineContext& context) {
  const svp::audio::AudioStagePlan audio_plan =
      svp::audio::build_audio_stage_plan(context.options.source_path,
                                         context.plan.probe,
                                         executable_exists(context.options.ffmpeg_path),
                                         context.options.ffmpeg_path,
                                         context.model_runtime_available);
  nlohmann::json audio_json = svp::audio::audio_stage_plan_to_json(audio_plan);
  const svp::audio::AudioExtractionRun extraction_run =
      svp::audio::execute_audio_extraction_plan(audio_plan.extraction_plan,
                                                context.staging_dir,
                                                !context.options.verbose);
  nlohmann::json extraction_run_json =
      svp::audio::audio_extraction_run_to_json(extraction_run);
  const svp::audio::VadExecutionBoundary vad_boundary =
      svp::audio::build_vad_execution_boundary(audio_plan.vad_task_plan,
                                               extraction_run.analysis_audio_written,
                                               extraction_run.waveform_written,
                                               context.model_runtime_available);
  const svp::audio::VadExecutionBoundary executed_boundary =
      svp::audio::execute_vad_boundary(vad_boundary, context.staging_dir);

  audio_json["audio_extraction"]["execution"] = extraction_run_json;
  audio_json["audio_extraction"]["extraction_run"] =
      extraction_run.extraction_run;
  audio_json["audio_extraction"]["original_streams_written"] =
      extraction_run.original_streams_written;
  audio_json["audio_extraction"]["analysis_audio_written"] =
      extraction_run.analysis_audio_written;
  audio_json["audio_extraction"]["audio_absence_written"] =
      extraction_run.audio_absence_written;
  audio_json["audio_extraction"]["waveform_written"] =
      extraction_run.waveform_written;
  audio_json["audio_extraction"]["processor_provenance_written"] =
      extraction_run.processor_provenance_written;
  for (const std::string& blocker : extraction_run.blockers) {
    audio_json["blockers"].push_back(blocker);
  }
  for (const std::string& blocker : executed_boundary.blockers) {
    if (std::find(audio_json["blockers"].begin(), audio_json["blockers"].end(), blocker) == audio_json["blockers"].end()) {
      audio_json["blockers"].push_back(blocker);
    }
  }
  audio_json["vad_execution_boundary"] =
      svp::audio::vad_execution_boundary_to_json(executed_boundary);

  std::int64_t media_duration_us = 0;
  if (context.plan.probe.container_timing.has_value() &&
      context.plan.probe.container_timing->duration_pts.has_value()) {
    media_duration_us = svp::media::pts_to_microseconds(
        *context.plan.probe.container_timing->duration_pts,
        context.plan.probe.container_timing->timebase);
  } else if (!context.plan.probe.audio_streams.empty() &&
             context.plan.probe.audio_streams.front().timing.duration_pts.has_value()) {
    media_duration_us = svp::media::pts_to_microseconds(
        *context.plan.probe.audio_streams.front().timing.duration_pts,
        context.plan.probe.audio_streams.front().timing.timebase);
  }

  const svp::audio::AsrChunkPlanResult asr_chunk_plan =
      svp::audio::build_asr_chunk_plan(media_duration_us);

  const std::filesystem::path model_cache_root =
      context.options.model_cache_dir.empty()
          ? std::filesystem::path{}
          : std::filesystem::path(context.options.model_cache_dir);

  const bool asr_model_available =
      svp::audio::check_asr_model_in_cache(
          "model_whisper_small_en", model_cache_root);

  const bool asr_model_verified =
      asr_model_available &&
      svp::audio::verify_asr_model_files(
          "model_whisper_small_en", model_cache_root);

  const svp::audio::AsrExecutionBoundary asr_boundary =
      svp::audio::build_asr_execution_boundary(
          asr_chunk_plan,
          extraction_run.analysis_audio_written,
          context.model_runtime_available,
          asr_model_available,
          asr_model_verified);

  const svp::audio::AsrExecutionBoundary executed_asr_boundary =
      svp::audio::execute_asr_boundary(
          asr_boundary, context.staging_dir, model_cache_root,
          [&context](std::size_t current, std::size_t total) {
            if (total > 0) {
              emit_stage_progress(context, ProgressStageId::asr,
                                  static_cast<std::uint64_t>(current),
                                  static_cast<std::uint64_t>(total),
                                  "chunks");
            }
          });

  // Diarization boundary: check for diarization model in cache.
  // If unavailable, honest fallback one-speaker segment is produced.
  // When force_single_speaker is set, skip all Sherpa checks and execution.
  const bool diar_model_available =
      context.options.force_single_speaker ||
      svp::audio::check_diarization_model_in_cache(
          "model_sherpa_onnx_diarization", model_cache_root);
  const bool diar_model_verified =
      context.options.force_single_speaker ||
      (diar_model_available &&
       svp::audio::verify_diarization_model_files(
           "model_sherpa_onnx_diarization", model_cache_root));

  svp::audio::DiarizationExecutionBoundary diar_boundary =
      svp::audio::build_diarization_boundary(
          extraction_run.analysis_audio_written,
          context.model_runtime_available,
          diar_model_available,
          diar_model_verified,
          media_duration_us);
  // Check sherpa-onnx availability AFTER ASR has loaded its models.
  // Loading sherpa's dylib (which bundles its own onnxruntime) before
  // ASR model loading corrupts the ONNX schema registry.
  // Skip Sherpa availability check entirely when force_single_speaker is set.
  if (!context.options.force_single_speaker &&
      !svp::audio::is_sherpa_diarization_available() &&
      !context.options.allow_fallback_diarization) {
    std::cerr << "\n  ERROR: sherpa-onnx C API library not found.\n"
              << "  Diarization cannot run. Speaker detection will NOT be performed.\n\n"
              << "  To fix:\n"
              << "    pip3 install sherpa-onnx\n"
              << "  Or set SHERPA_ONNX_LIB_PATH to the library path.\n"
              << "  Or use --sherpa-lib <path> to specify it explicitly.\n\n"
              << "  To proceed WITHOUT diarization (NOT RECOMMENDED):\n"
              << "    --allow-fallback-diarization\n\n"
              << "  Or to skip diarization intentionally:\n"
              << "    --force-single-speaker\n\n";
    return 1;
  }
  if (!context.options.force_single_speaker &&
      context.options.allow_fallback_diarization &&
      !svp::audio::is_sherpa_diarization_available()) {
    std::cerr << "  WARNING: --allow-fallback-diarization is active. "
              << "sherpa-onnx is not available. "
              << "Speaker data will be FABRICATED FALLBACK, not real.\n";
    emit_warning(context, ProgressStageId::diarization,
                 "Fallback diarization active; sherpa-onnx not available. "
                 "Speaker data is fabricated fallback, not real.");
  }

  diar_boundary = svp::audio::execute_diarization_boundary(
      std::move(diar_boundary), context.staging_dir, model_cache_root,
      context.options.allow_fallback_diarization,
      context.options.force_single_speaker);

  if (context.stage_plan.run_audio &&
      diar_boundary.diarization_status == svp::audio::DiarizationStatus::unavailable &&
      !context.options.allow_fallback_diarization &&
      !context.options.force_single_speaker) {
    std::cerr << "\n  ERROR: Diarization is unavailable. Speaker detection will NOT be performed.\n"
              << "  Cause:";
    for (const auto& blocker : diar_boundary.blockers) {
      std::cerr << "\n    - " << blocker;
    }
    std::cerr << "\n\n  To fix:\n"
              << "    pip3 install sherpa-onnx\n"
              << "    Ensure the diarization model is in the model cache.\n"
              << "    Ensure analysis audio was extracted successfully.\n\n"
              << "  To proceed WITHOUT diarization (NOT RECOMMENDED):\n"
              << "    --allow-fallback-diarization\n\n";
    return 1;
  }

  // Serialize diarization boundary before moving segments out.
  audio_json["diarization_execution_boundary"] =
      svp::audio::diarization_execution_boundary_to_json(diar_boundary);

  // Feed diarization results into the ASR boundary so transcript
  // artifacts carry diarization status and speaker segments.
  svp::audio::AsrExecutionBoundary asr_with_diar = executed_asr_boundary;
  asr_with_diar.diarization_status =
      svp::audio::diarization_status_to_string(diar_boundary.diarization_status);
  asr_with_diar.diarization_blockers = diar_boundary.blockers;
  if (diar_boundary.diarization_status == svp::audio::DiarizationStatus::user_declared_single_speaker) {
    asr_with_diar.one_speaker_mode = true;
    asr_with_diar.speaker_count = 1;
    asr_with_diar.diarization_note =
        "User requested single-speaker mode; diarization was intentionally skipped.";
  } else if (diar_boundary.diarization_status == svp::audio::DiarizationStatus::fallback_one_speaker) {
    asr_with_diar.one_speaker_mode = true;
    asr_with_diar.speaker_count = 1;
    if (!diar_boundary.blockers.empty()) {
      std::string note = "One-speaker fallback used (";
      for (std::size_t i = 0; i < diar_boundary.blockers.size(); ++i) {
        if (i > 0) note += "; ";
        note += diar_boundary.blockers[i];
      }
      note += "). This is not speaker recognition.";
      asr_with_diar.diarization_note = std::move(note);
    } else {
      asr_with_diar.diarization_note =
          "One-speaker fallback used. This is not speaker recognition.";
    }
  } else if (diar_boundary.diarization_status == svp::audio::DiarizationStatus::ran) {
    asr_with_diar.one_speaker_mode = false;
    asr_with_diar.speaker_count = diar_boundary.speaker_count;
  } else {
    if (!diar_boundary.blockers.empty()) {
      std::string note = "Diarization did not run (";
      for (std::size_t i = 0; i < diar_boundary.blockers.size(); ++i) {
        if (i > 0) note += "; ";
        note += diar_boundary.blockers[i];
      }
      note += ").";
      asr_with_diar.diarization_note = std::move(note);
    }
  }
  asr_with_diar.speaker_segments = std::move(diar_boundary.speaker_segments);

  if (diar_boundary.diarization_status == svp::audio::DiarizationStatus::ran &&
      !asr_with_diar.reconciled_words.empty()) {
    const std::filesystem::path wav_path =
        context.staging_dir / "media/audio/analysis_mono_16k.wav";
    const std::filesystem::path diar_model_dir =
        model_cache_root / diar_boundary.model_id;
    asr_with_diar.word_speaker_assignments =
        svp::audio::refine_word_speakers_by_embedding(
            wav_path, diar_model_dir,
            asr_with_diar.reconciled_words,
            diar_boundary.raw_diar_result);
  }

  const svp::audio::TranscriptWriteResult transcript_result =
      svp::audio::write_transcript_artifacts(asr_with_diar, context.staging_dir);

  audio_json["asr_chunk_plan"] =
      svp::audio::asr_chunk_plan_to_json(asr_with_diar.chunk_plan);
  audio_json["asr_execution_boundary"] =
      svp::audio::asr_execution_boundary_to_json(asr_with_diar);
  audio_json["transcript_write_result"] =
      svp::audio::transcript_write_result_to_json(transcript_result);
  for (const std::string& blocker : asr_with_diar.blockers) {
    if (std::find(audio_json["blockers"].begin(), audio_json["blockers"].end(),
                  blocker) == audio_json["blockers"].end()) {
      audio_json["blockers"].push_back(blocker);
    }
  }
  for (const std::string& blocker : diar_boundary.blockers) {
    if (std::find(audio_json["blockers"].begin(), audio_json["blockers"].end(),
                  blocker) == audio_json["blockers"].end()) {
      audio_json["blockers"].push_back(blocker);
    }
  }

  context.output["audio_foundation"] = audio_json;
  return std::nullopt;
}

}  // namespace svp::builder
