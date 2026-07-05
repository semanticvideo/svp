#include "svp/audio/diarization_boundary.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/models/manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace svp::audio {
namespace {

std::string segment_id_for_ordinal(std::size_t ordinal) {
  std::string id = "speakerseg_";
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%06zu", ordinal);
  id += buf;
  return id;
}

}  // namespace

bool check_diarization_model_in_cache(const std::string& model_id,
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

bool verify_diarization_model_files(const std::string& model_id,
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

std::string diarization_status_to_string(DiarizationStatus status) {
  switch (status) {
    case DiarizationStatus::planned: return "planned";
    case DiarizationStatus::unavailable: return "unavailable";
    case DiarizationStatus::ran: return "ran";
    case DiarizationStatus::fallback_one_speaker: return "fallback_one_speaker";
    case DiarizationStatus::user_declared_single_speaker: return "user_declared_single_speaker";
  }
  return "unknown";
}

DiarizationExecutionBoundary build_diarization_boundary(
    bool analysis_audio_available,
    bool model_runtime_available,
    bool model_available,
    bool model_verified,
    std::int64_t total_duration_us) {
  DiarizationExecutionBoundary boundary;
  boundary.analysis_audio_available = analysis_audio_available;
  boundary.model_runtime_available = model_runtime_available;
  boundary.model_available = model_available;
  boundary.model_verified = model_verified;
  boundary.total_duration_us = total_duration_us;

  if (!analysis_audio_available) {
    boundary.blockers.push_back("analysis audio is not staged for diarization execution");
  }
  if (!model_runtime_available) {
    boundary.blockers.push_back("ONNX Runtime is not available for diarization execution");
  }
  if (!model_available) {
    boundary.blockers.push_back("sherpa-onnx diarization model is not available in model cache");
  }
  if (model_available && !model_verified) {
    boundary.blockers.push_back("diarization model manifest/required files could not be verified");
  }

  if (!boundary.blockers.empty()) {
    boundary.diarization_status = DiarizationStatus::unavailable;
  }

  return boundary;
}

std::string speaker_id_for_cluster(int32_t cluster_id) {
  std::ostringstream output;
  output << "speaker_" << std::setw(4) << std::setfill('0') << (cluster_id + 1);
  return output.str();
}

DiarizationExecutionBoundary execute_diarization_boundary(
    DiarizationExecutionBoundary boundary,
    const std::filesystem::path& staging_root,
    const std::filesystem::path& model_cache_root,
    bool allow_fallback,
    bool force_single_speaker,
    const std::vector<AsrWord>& words) {
  if (force_single_speaker) {
    SpeakerSegment single_segment;
    single_segment.id = segment_id_for_ordinal(0);
    single_segment.speaker_id = "speaker_0001";
    single_segment.timing = {0, boundary.total_duration_us};
    single_segment.confidence = 0.0;
    single_segment.overlap = false;
    boundary.speaker_segments.push_back(std::move(single_segment));
    boundary.speaker_count = 1;
    boundary.diarization_status = DiarizationStatus::user_declared_single_speaker;
    boundary.blockers.clear();
    return boundary;
  }

  if (boundary.diarization_status == DiarizationStatus::unavailable) {
    if (allow_fallback && boundary.total_duration_us > 0 && boundary.analysis_audio_available) {
      SpeakerSegment fallback_segment;
      fallback_segment.id = segment_id_for_ordinal(0);
      fallback_segment.speaker_id = "speaker_0001";
      fallback_segment.timing = {0, boundary.total_duration_us};
      fallback_segment.confidence = 0.0;
      fallback_segment.overlap = false;
      boundary.speaker_segments.push_back(std::move(fallback_segment));
      boundary.speaker_count = 1;
      boundary.diarization_status = DiarizationStatus::fallback_one_speaker;
    } else {
      boundary.diarization_status = DiarizationStatus::unavailable;
    }
    return boundary;
  }

  // Model is available and verified — attempt real diarization inference.
  const std::filesystem::path wav_path =
      staging_root / "media/audio/analysis_mono_16k.wav";
  const std::filesystem::path model_dir = model_cache_root / boundary.model_id;

  if (!std::filesystem::exists(wav_path)) {
    boundary.blockers.push_back("analysis WAV not found for diarization: " + wav_path.string());
    if (allow_fallback) {
      SpeakerSegment fallback_segment;
      fallback_segment.id = segment_id_for_ordinal(0);
      fallback_segment.speaker_id = "speaker_0001";
      fallback_segment.timing = {0, boundary.total_duration_us};
      fallback_segment.confidence = 0.0;
      fallback_segment.overlap = false;
      boundary.speaker_segments.push_back(std::move(fallback_segment));
      boundary.speaker_count = 1;
      boundary.diarization_status = DiarizationStatus::fallback_one_speaker;
    } else {
      boundary.diarization_status = DiarizationStatus::unavailable;
    }
    return boundary;
  }

  if (!svp::audio::is_sherpa_diarization_available()) {
    std::string blocker_msg =
        "sherpa-onnx C API library not loaded; ";
    if (allow_fallback) {
      blocker_msg += "fallback one-speaker segment emitted. ";
    } else {
      blocker_msg += "diarization cannot run. ";
    }
    blocker_msg += "Attempted paths:";
    std::vector<std::string> attempted = svp::audio::sherpa_lib_paths_attempted();
    if (attempted.empty()) {
      blocker_msg += " (none — library discovery was not triggered)";
    } else {
      for (std::size_t i = 0; i < attempted.size(); ++i) {
        blocker_msg += "\n  [" + std::to_string(i + 1) + "] " + attempted[i];
      }
    }
    boundary.blockers.push_back(blocker_msg);
    if (allow_fallback) {
      SpeakerSegment fallback_segment;
      fallback_segment.id = segment_id_for_ordinal(0);
      fallback_segment.speaker_id = "speaker_0001";
      fallback_segment.timing = {0, boundary.total_duration_us};
      fallback_segment.confidence = 0.0;
      fallback_segment.overlap = false;
      boundary.speaker_segments.push_back(std::move(fallback_segment));
      boundary.speaker_count = 1;
      boundary.diarization_status = DiarizationStatus::fallback_one_speaker;
    } else {
      boundary.diarization_status = DiarizationStatus::unavailable;
    }
    return boundary;
  }

  SherpaDiarizationResult diar_result =
      run_sherpa_diarization(wav_path, model_dir, words);

  if (!diar_result.ran) {
    for (const auto& blocker : diar_result.blockers) {
      boundary.blockers.push_back(blocker);
    }
    if (allow_fallback) {
      SpeakerSegment fallback_segment;
      fallback_segment.id = segment_id_for_ordinal(0);
      fallback_segment.speaker_id = "speaker_0001";
      fallback_segment.timing = {0, boundary.total_duration_us};
      fallback_segment.confidence = 0.0;
      fallback_segment.overlap = false;
      boundary.speaker_segments.push_back(std::move(fallback_segment));
      boundary.speaker_count = 1;
      boundary.diarization_status = DiarizationStatus::fallback_one_speaker;
    } else {
      boundary.diarization_status = DiarizationStatus::unavailable;
    }
    return boundary;
  }

  // Real diarization succeeded — convert segments to SVP format.
  for (std::size_t i = 0; i < diar_result.segments.size(); ++i) {
    const auto& seg = diar_result.segments[i];
    SpeakerSegment ssvp_seg;
    ssvp_seg.id = segment_id_for_ordinal(i);
    ssvp_seg.speaker_id = speaker_id_for_cluster(seg.speaker_id);
    ssvp_seg.timing = {
        static_cast<std::int64_t>(seg.start_sec * 1000000.0f),
        static_cast<std::int64_t>(seg.end_sec * 1000000.0f)
    };
    ssvp_seg.confidence = 1.0;
    ssvp_seg.overlap = false;
    boundary.speaker_segments.push_back(std::move(ssvp_seg));
  }
  boundary.speaker_count = static_cast<std::size_t>(diar_result.final_speaker_count);
  boundary.diarization_status = DiarizationStatus::ran;
  boundary.reconciliation_method = diar_result.reconciliation_method;
  boundary.preliminary_cluster_count = diar_result.preliminary_cluster_count;
  boundary.word_speaker_assignments = diar_result.word_speaker_assignments;

  // Store similarity matrix and merge decisions as JSON in the boundary
  nlohmann::json sim_matrix_json = nlohmann::json::array();
  for (const auto& row : diar_result.pairwise_similarity_matrix) {
    sim_matrix_json.push_back(nlohmann::json(row));
  }
  boundary.pairwise_similarity_matrix_json = sim_matrix_json;

  nlohmann::json merge_decisions_json = nlohmann::json::array();
  for (const auto& md : diar_result.merge_decisions) {
    merge_decisions_json.push_back({
        {"cluster_a", md.cluster_a},
        {"cluster_b", md.cluster_b},
        {"cosine_similarity", md.cosine_similarity},
        {"merged", md.merged}
    });
  }
  boundary.merge_decisions_json = merge_decisions_json;

  boundary.raw_diar_result = std::move(diar_result);

  return boundary;
}

nlohmann::json diarization_execution_boundary_to_json(
    const DiarizationExecutionBoundary& boundary) {
  nlohmann::json segments_json = nlohmann::json::array();
  for (const SpeakerSegment& seg : boundary.speaker_segments) {
    segments_json.push_back(speaker_segment_to_json(seg));
  }

  nlohmann::json result = {
      {"processor_id", boundary.processor_id},
      {"model_id", boundary.model_id},
      {"runtime", boundary.runtime},
      {"execution_provider", boundary.execution_provider},
      {"speaker_segments_output_ref", boundary.speaker_segments_output_ref},
      {"analysis_audio_available", boundary.analysis_audio_available},
      {"model_runtime_available", boundary.model_runtime_available},
      {"model_available", boundary.model_available},
      {"model_verified", boundary.model_verified},
      {"diarization_status", diarization_status_to_string(boundary.diarization_status)},
      {"speaker_count", boundary.speaker_count},
      {"total_duration_us", boundary.total_duration_us},
      {"speaker_segments", segments_json},
      {"blockers", boundary.blockers},
  };

  if (!boundary.reconciliation_method.empty()) {
    result["reconciliation_method"] = boundary.reconciliation_method;
    result["preliminary_cluster_count"] = boundary.preliminary_cluster_count;
  }
  if (!boundary.pairwise_similarity_matrix_json.is_null()) {
    result["pairwise_similarity_matrix"] = boundary.pairwise_similarity_matrix_json;
  }
  if (!boundary.merge_decisions_json.is_null()) {
    result["merge_decisions"] = boundary.merge_decisions_json;
  }

  return result;
}

}  // namespace svp::audio
