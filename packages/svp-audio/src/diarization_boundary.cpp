#include "svp/audio/diarization_boundary.hpp"
#include "svp/models/manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
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

DiarizationExecutionBoundary execute_diarization_boundary(
    DiarizationExecutionBoundary boundary) {
  if (boundary.diarization_status == DiarizationStatus::unavailable) {
    if (boundary.total_duration_us > 0 && boundary.analysis_audio_available) {
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

  // When a real diarization model is available, this is where sherpa-onnx
  // inference would run. For now, the boundary is prepared but the actual
  // model execution is not wired. We fall back to one-speaker mode so that
  // the pipeline produces honest, valid artifacts.
  if (boundary.total_duration_us > 0) {
    SpeakerSegment fallback_segment;
    fallback_segment.id = segment_id_for_ordinal(0);
    fallback_segment.speaker_id = "speaker_0001";
    fallback_segment.timing = {0, boundary.total_duration_us};
    fallback_segment.confidence = 0.0;
    fallback_segment.overlap = false;
    boundary.speaker_segments.push_back(std::move(fallback_segment));
    boundary.speaker_count = 1;
    boundary.diarization_status = DiarizationStatus::fallback_one_speaker;
    boundary.blockers.push_back(
        "sherpa-onnx diarization model inference is not yet wired; "
        "fallback one-speaker segment emitted");
  } else {
    boundary.diarization_status = DiarizationStatus::unavailable;
  }

  return boundary;
}

nlohmann::json diarization_execution_boundary_to_json(
    const DiarizationExecutionBoundary& boundary) {
  nlohmann::json segments_json = nlohmann::json::array();
  for (const SpeakerSegment& seg : boundary.speaker_segments) {
    segments_json.push_back(speaker_segment_to_json(seg));
  }

  return {
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
}

}  // namespace svp::audio
