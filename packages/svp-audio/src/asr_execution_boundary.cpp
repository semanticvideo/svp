#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/asr_chunk_context.hpp"
#include "svp/audio/transcript_records.hpp"
#include "svp/audio/wav_slice.hpp"
#include "svp/audio/whisper_model.hpp"
#include "svp/core/memory_diagnostics.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace svp::audio {
namespace {

void write_json_file(const std::filesystem::path& path, const nlohmann::json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to open staged ASR JSON artifact: " + path.string());
  }
  output << value.dump(2) << "\n";
}

void write_jsonl_file(const std::filesystem::path& path,
                      const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to open staged ASR JSONL artifact: " + path.string());
  }
  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

void write_empty_jsonl(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to open staged ASR JSONL artifact: " + path.string());
  }
}

std::string word_id_for_ordinal(std::size_t ordinal) {
  std::ostringstream output;
  output << "word_" << std::setw(6) << std::setfill('0') << ordinal;
  return output.str();
}

nlohmann::json transcript_summary_json(std::int64_t duration_us,
                                       std::size_t word_count,
                                       std::size_t speaker_count,
                                       const std::string& source_audio_id,
                                       const std::string& processor_id,
                                       bool blocked) {
  nlohmann::json language;
  if (blocked || word_count == 0) {
    language = {
        {"primary", "und"},
        {"detected", nlohmann::json::array()},
        {"mode", "undetermined"},
        {"confidence", 0.0},
    };
  } else {
    language = {
        {"primary", "en"},
        {"detected", {"en"}},
        {"mode", "single"},
        {"confidence", 0.0},
    };
  }

  return {
      {"language", language},
      {"duration_us", duration_us},
      {"word_count", word_count},
      {"speaker_count", speaker_count},
      {"source_audio_id", source_audio_id},
      {"processor_id", processor_id},
  };
}

nlohmann::json speaker_record_json(const std::string& speaker_id,
                                   const std::string& processor_id,
                                   std::int64_t total_speech_us) {
  return {
      {"id", speaker_id},
      {"display_name", "Speaker 1"},
      {"total_speech_us", total_speech_us},
      {"confidence", 0.0},
      {"processor_id", processor_id},
  };
}

nlohmann::json chunk_provenance_record(const AsrChunkPlan& chunk,
                                       const std::string& processor_id,
                                       const std::string& asr_status) {
  nlohmann::json asr_limitations = {
      {"timestamp_method", "whisper_cpp_token_timestamps"},
      {"timestamp_precision", "centisecond_token_boundaries"},
      {"confidence_status", "whisper_cpp_token_probability_mean"},
      {"speaker_mode", "one_speaker_fallback"},
  };

  return {
      {"chunk_id", chunk.chunk_id},
      {"processor_id", processor_id},
      {"source_start_us", chunk.source_start_us},
      {"source_end_us", chunk.source_end_us},
      {"overlap_before_us", chunk.overlap_before_us},
      {"overlap_after_us", chunk.overlap_after_us},
      {"model_id", chunk.model_id},
      {"runtime", chunk.runtime},
      {"asr_status", asr_status},
      {"asr_limitations", asr_limitations},
  };
}

}  // namespace

bool check_asr_model_in_cache(const std::string& model_id,
                              const std::filesystem::path& model_cache_root) {
  if (model_cache_root.empty()) {
    return false;
  }

  const std::filesystem::path model_dir = model_cache_root / model_id;
  if (!std::filesystem::exists(model_dir)) {
    return false;
  }

  const std::filesystem::path manifest_path = model_dir / "model.svpmodel.json";
  if (!std::filesystem::exists(manifest_path)) {
    return false;
  }

  return true;
}

bool verify_asr_model_files(const std::string& model_id,
                            const std::filesystem::path& model_cache_root) {
  if (model_cache_root.empty()) {
    return false;
  }

  const std::filesystem::path model_dir = model_cache_root / model_id;
  const std::filesystem::path manifest_path = model_dir / "model.svpmodel.json";
  if (!std::filesystem::exists(manifest_path)) {
    return false;
  }

  try {
    const svp::models::ModelBundleManifest manifest =
        svp::models::load_model_bundle_manifest(manifest_path);

    for (const auto& file : manifest.files) {
      const std::filesystem::path file_path = model_dir / file.path;
      if (!std::filesystem::exists(file_path)) {
        return false;
      }
    }

    return true;
  } catch (...) {
    return false;
  }
}

std::string asr_status_to_string(AsrStatus status) {
  switch (status) {
    case AsrStatus::planned: return "planned";
    case AsrStatus::blocked: return "blocked";
    case AsrStatus::ran: return "ran";
  }
  return "unknown";
}

AsrExecutionBoundary build_asr_execution_boundary(const AsrChunkPlanResult& chunk_plan,
                                                   bool analysis_audio_available,
                                                   bool model_runtime_available,
                                                   bool model_available,
                                                   bool model_verified,
                                                   const std::string& analysis_audio_ref) {
  AsrExecutionBoundary boundary;
  boundary.chunk_plan = chunk_plan;
  boundary.analysis_audio_available = analysis_audio_available;
  boundary.model_runtime_available = model_runtime_available;
  boundary.model_available = model_available;
  boundary.model_verified = model_verified;

  boundary.input_refs = {analysis_audio_ref};

  for (const AsrChunkPlan& chunk : chunk_plan.chunks) {
    boundary.staged_chunk_output_refs.push_back(chunk.output_ref);
  }

  if (!analysis_audio_available) {
    boundary.blockers.push_back("analysis audio is not staged for ASR execution");
  }
  if (!model_runtime_available) {
    boundary.blockers.push_back(
        "whisper.cpp runtime is not available for ASR execution");
  }
  if (!model_available) {
    boundary.blockers.push_back("Whisper ASR model is not available in model cache");
  }
  if (model_available && !model_verified) {
    boundary.blockers.push_back("Whisper ASR model manifest/required files could not be verified");
  }
  if (chunk_plan.chunks.empty()) {
    boundary.blockers.push_back("no ASR chunks are available to process");
  }

  if (!boundary.blockers.empty()) {
    boundary.asr_status = AsrStatus::blocked;
  }

  return boundary;
}

AsrExecutionBoundary execute_asr_boundary(AsrExecutionBoundary boundary,
                                          const std::filesystem::path& staging_root,
                                          const std::filesystem::path& model_cache_root,
                                          AsrChunkProgressCallback on_chunk_progress) {
  if (!boundary.blockers.empty()) {
    boundary.asr_status = AsrStatus::blocked;
    boundary.transcript_written = false;
    boundary.words_written = false;
    boundary.speakers_written = false;
    boundary.chunk_provenance_written = false;
    return boundary;
  }

  try {
    const std::filesystem::path model_dir = model_cache_root / boundary.model_id;
    if (boundary.input_refs.size() != 1 || boundary.input_refs.front().empty()) {
      throw std::runtime_error("ASR execution requires exactly one analysis audio input_ref");
    }
    const std::filesystem::path input_wav = staging_root / boundary.input_refs.front();
    if (!std::filesystem::exists(input_wav)) {
      throw std::runtime_error("staged analysis WAV file not found: " + input_wav.string());
    }

    const std::filesystem::path temp_slice_dir =
        std::filesystem::temp_directory_path() / "svp-asr-chunk-slices";

    std::vector<std::vector<AsrWord>> chunk_words;
    chunk_words.reserve(boundary.chunk_plan.chunks.size());
    svp::core::check_memory_limit("asr.boundary.begin", {
        {"chunk_count", std::to_string(boundary.chunk_plan.chunks.size())},
        {"input_wav", input_wav.string()},
        {"model_dir", model_dir.string()}
    });

    for (std::size_t i = 0; i < boundary.chunk_plan.chunks.size(); ++i) {
      const AsrChunkPlan& chunk = boundary.chunk_plan.chunks[i];
      if (i == 0 || ((i + 1) % 10) == 0 ||
          i + 1 == boundary.chunk_plan.chunks.size()) {
        svp::core::check_memory_limit("asr.chunk.begin", {
            {"index", std::to_string(i)},
            {"chunk_id", chunk.chunk_id},
            {"total", std::to_string(boundary.chunk_plan.chunks.size())},
            {"start_us", std::to_string(chunk.source_start_us)},
            {"end_us", std::to_string(chunk.source_end_us)}
        });
      }

      if (on_chunk_progress) {
        on_chunk_progress(i, boundary.chunk_plan.chunks.size());
      }

      const AsrChunkContextPlan context = plan_asr_chunk_context(chunk);
      const std::filesystem::path chunk_wav =
          slice_wav_to_temp(input_wav, context.slice_start_us,
                            context.slice_end_us, temp_slice_dir);

      WhisperInferenceResult whisper_result;
      try {
        whisper_result = run_whisper_inference(
            chunk_wav, model_dir, chunk.chunk_id, 0,
            context.slice_end_us - context.slice_start_us);
      } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(chunk_wav, cleanup_error);
        throw;
      }
      std::error_code cleanup_error;
      std::filesystem::remove(chunk_wav, cleanup_error);
      if (i == 0 || ((i + 1) % 10) == 0 ||
          i + 1 == boundary.chunk_plan.chunks.size()) {
        svp::core::check_memory_limit("asr.chunk.after_inference", {
            {"index", std::to_string(i)},
            {"chunk_id", chunk.chunk_id},
            {"ran", whisper_result.ran ? "true" : "false"},
            {"word_count", std::to_string(whisper_result.all_words.size())}
        });
      }

      if (!whisper_result.ran) {
        for (const std::string& blocker : whisper_result.blockers) {
          if (std::find(boundary.blockers.begin(), boundary.blockers.end(), blocker) ==
              boundary.blockers.end()) {
            boundary.blockers.push_back(blocker);
          }
        }
        chunk_words.push_back({});
        continue;
      }

      chunk_words.push_back(retain_nominal_chunk_words(
          whisper_result.all_words, context, chunk,
          static_cast<std::int64_t>(i)));
    }
    svp::core::check_memory_limit("asr.boundary.after_chunks", {
        {"chunk_count", std::to_string(boundary.chunk_plan.chunks.size())},
        {"chunk_word_vectors", std::to_string(chunk_words.size())}
    });

    if (on_chunk_progress) {
      on_chunk_progress(boundary.chunk_plan.chunks.size(),
                        boundary.chunk_plan.chunks.size());
    }

    if (!boundary.blockers.empty()) {
      boundary.asr_status = AsrStatus::blocked;
      boundary.transcript_written = false;
      boundary.words_written = false;
      boundary.speakers_written = false;
      boundary.chunk_provenance_written = false;
      return boundary;
    }

    boundary.reconciled_words =
        reconcile_overlapping_chunks(chunk_words, boundary.chunk_plan.chunks);
    boundary.raw_word_count = 0;
    for (const auto& cw : chunk_words) {
      boundary.raw_word_count += cw.size();
    }
    boundary.reconciled_word_count = boundary.reconciled_words.size();
    boundary.speaker_count = 1;
    boundary.asr_status = AsrStatus::ran;
    svp::core::check_memory_limit("asr.boundary.complete", {
        {"raw_word_count", std::to_string(boundary.raw_word_count)},
        {"reconciled_word_count", std::to_string(boundary.reconciled_word_count)}
    });
  } catch (const std::exception& error) {
    boundary.asr_status = AsrStatus::blocked;
    boundary.blockers.push_back(std::string("ASR execution blocked: ") + error.what());
    boundary.transcript_written = false;
    boundary.words_written = false;
    boundary.speakers_written = false;
    boundary.chunk_provenance_written = false;
  }

  return boundary;
}

nlohmann::json asr_execution_boundary_to_json(const AsrExecutionBoundary& boundary) {
  nlohmann::json chunk_refs = nlohmann::json::array();
  for (const std::string& ref : boundary.staged_chunk_output_refs) {
    chunk_refs.push_back(ref);
  }

  nlohmann::json asr_limitations = {
      {"timestamp_method", "whisper_cpp_token_timestamps"},
      {"timestamp_precision", "centisecond_token_boundaries"},
      {"confidence_status", "whisper_cpp_token_probability_mean"},
      {"confidence_note", "Per-word confidence is the mean of selected-token decoder softmax probabilities for the word's constituent tokens. This is uncalibrated model confidence, not a calibrated probability."},
      {"speaker_mode", boundary.diarization_status == "fallback_one_speaker"
           ? "one_speaker_fallback"
           : (boundary.diarization_status == "user_declared_single_speaker"
                ? "user_declared_single_speaker"
                : (boundary.diarization_status == "microphone_stream_assignment"
                     ? "camera_microphone_stream_locked"
                     : "diarization_assigned"))},
  };

  nlohmann::json segments_json = nlohmann::json::array();
  for (const SpeakerSegment& seg : boundary.speaker_segments) {
    segments_json.push_back(speaker_segment_to_json(seg));
  }

  return {
      {"processor_id", boundary.processor_id},
      {"model_id", boundary.model_id},
      {"runtime", boundary.runtime},
      {"execution_provider", boundary.execution_provider},
      {"input_refs", boundary.input_refs},
      {"staged_chunk_output_refs", chunk_refs},
      {"transcript_output_ref", boundary.transcript_output_ref},
      {"words_output_ref", boundary.words_output_ref},
      {"speakers_output_ref", boundary.speakers_output_ref},
      {"chunk_provenance_ref", boundary.chunk_provenance_ref},
      {"analysis_audio_available", boundary.analysis_audio_available},
      {"model_runtime_available", boundary.model_runtime_available},
      {"model_available", boundary.model_available},
      {"model_verified", boundary.model_verified},
      {"asr_status", asr_status_to_string(boundary.asr_status)},
      {"transcript_written", boundary.transcript_written},
      {"words_written", boundary.words_written},
      {"speakers_written", boundary.speakers_written},
      {"chunk_provenance_written", boundary.chunk_provenance_written},
      {"raw_word_count", boundary.raw_word_count},
      {"reconciled_word_count", boundary.reconciled_word_count},
      {"speaker_count", boundary.speaker_count},
      {"one_speaker_mode", boundary.one_speaker_mode},
      {"diarization_status", boundary.diarization_status},
      {"diarization_processor_id", boundary.diarization_processor_id},
      {"diarization_note", boundary.diarization_note},
      {"diarization_blockers", boundary.diarization_blockers},
      {"speaker_segments", segments_json},
      {"speaker_source_audio_stream_ids",
       boundary.speaker_source_audio_stream_ids},
      {"speaker_source_audio_stream_groups",
       boundary.speaker_source_audio_stream_groups},
      {"asr_limitations", asr_limitations},
      {"blockers", boundary.blockers},
      {"chunk_plan", asr_chunk_plan_to_json(boundary.chunk_plan)},
  };
}

}  // namespace svp::audio
