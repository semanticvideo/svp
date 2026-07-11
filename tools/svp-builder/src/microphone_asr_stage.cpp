#include "microphone_asr_stage.hpp"

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/microphone_transcript.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/whisper_cpp_backend.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace svp::builder {
namespace {

std::vector<float> microphone_voice_fingerprint(
    const svp::audio::SherpaDiarizationResult& diarization) {
  std::map<int32_t, double> duration_by_speaker;
  for (const auto& segment : diarization.segments) {
    if (segment.speaker_id >= 0) {
      duration_by_speaker[segment.speaker_id] +=
          std::max(0.0f, segment.end_sec - segment.start_sec);
    }
  }

  std::vector<float> aggregate;
  double total_weight = 0.0;
  for (const auto& [speaker, duration] : duration_by_speaker) {
    if (duration <= 0.0 ||
        static_cast<std::size_t>(speaker) >=
            diarization.final_speaker_fingerprints.size()) {
      continue;
    }
    const auto& fingerprint =
        diarization.final_speaker_fingerprints[static_cast<std::size_t>(speaker)];
    if (fingerprint.empty()) continue;
    if (aggregate.empty()) aggregate.assign(fingerprint.size(), 0.0f);
    if (aggregate.size() != fingerprint.size()) continue;
    for (std::size_t dimension = 0; dimension < fingerprint.size(); ++dimension) {
      aggregate[dimension] +=
          fingerprint[dimension] * static_cast<float>(duration);
    }
    total_weight += duration;
  }
  if (aggregate.empty() || total_weight <= 0.0) return {};

  double norm_squared = 0.0;
  for (float value : aggregate) {
    norm_squared += static_cast<double>(value) * static_cast<double>(value);
  }
  const double norm = std::sqrt(norm_squared);
  if (norm <= 0.0) return {};
  for (float& value : aggregate) value /= static_cast<float>(norm);
  return aggregate;
}

std::vector<svp::audio::MicrophoneVoiceTrack> microphone_voice_tracks(
    const svp::audio::SherpaDiarizationResult& diarization) {
  std::vector<svp::audio::MicrophoneVoiceTrack> tracks;
  for (int32_t speaker = 0; speaker < diarization.final_speaker_count;
       ++speaker) {
    if (static_cast<std::size_t>(speaker) >=
        diarization.final_speaker_fingerprints.size()) {
      continue;
    }
    const auto& fingerprint =
        diarization.final_speaker_fingerprints[static_cast<std::size_t>(speaker)];
    if (fingerprint.empty()) continue;
    svp::audio::MicrophoneVoiceTrack track;
    track.track_ordinal = static_cast<std::size_t>(speaker);
    track.fingerprint = fingerprint;
    for (const auto& segment : diarization.segments) {
      if (segment.speaker_id != speaker || segment.end_sec <= segment.start_sec) {
        continue;
      }
      track.speech_segments.push_back({
          static_cast<std::int64_t>(std::llround(segment.start_sec * 1000000.0)),
          static_cast<std::int64_t>(std::llround(segment.end_sec * 1000000.0)),
      });
    }
    if (!track.speech_segments.empty()) tracks.push_back(std::move(track));
  }
  return tracks;
}

std::vector<svp::audio::TimeSpan> diarized_speech_spans(
    const std::vector<svp::audio::MicrophoneVoiceTrack>& tracks) {
  std::vector<svp::audio::TimeSpan> spans;
  for (const auto& track : tracks) {
    spans.insert(spans.end(), track.speech_segments.begin(),
                 track.speech_segments.end());
  }
  return spans;
}

}  // namespace

MicrophoneAsrStageResult run_microphone_asr_stage(
    const svp::audio::AudioExtractionPlan& extraction_plan,
    const svp::audio::AudioExtractionRun& extraction_run,
    std::int64_t media_duration_us,
    bool model_runtime_available,
    bool asr_model_available,
    bool asr_model_verified,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& model_cache_root,
    MicrophoneAsrProgressCallback progress,
    MicrophoneDiarizationProgressCallbacks diarization_progress) {
  MicrophoneAsrStageResult result;
  std::vector<svp::audio::AsrExecutionBoundary> boundaries;
  std::vector<svp::audio::MicrophoneTranscript> transcripts;
  std::vector<std::size_t> chunks_by_stream;
  std::size_t total_chunks = 0;
  for (const auto& microphone : extraction_plan.microphone_analysis_streams) {
    const std::int64_t duration_us =
        microphone.timeline_duration_us.value_or(media_duration_us);
    const std::size_t chunk_count =
        svp::audio::build_asr_chunk_plan(duration_us).chunks.size();
    chunks_by_stream.push_back(chunk_count);
    total_chunks += chunk_count;
  }
  std::size_t completed_chunks_before_stream = 0;
  std::vector<std::string> fingerprint_blockers;

  // Deliberately sequential: a microphone's ASR completes before the next
  // microphone begins.
  for (std::size_t stream_ordinal = 0;
       stream_ordinal < extraction_plan.microphone_analysis_streams.size();
       ++stream_ordinal) {
    const auto& microphone_plan =
        extraction_plan.microphone_analysis_streams[stream_ordinal];
    const bool microphone_available =
        stream_ordinal < extraction_run.microphone_analysis_streams.size() &&
        extraction_run.microphone_analysis_streams[stream_ordinal].success;
    const std::int64_t microphone_duration_us =
        microphone_plan.timeline_duration_us.value_or(media_duration_us);
    const svp::audio::AsrChunkPlanResult chunk_plan =
        svp::audio::build_asr_chunk_plan(
            microphone_duration_us, svp::audio::kDefaultAsrChunkDurationUs,
            svp::audio::kDefaultAsrChunkOverlapUs, microphone_plan.output_ref);
    const svp::audio::AsrExecutionBoundary boundary =
        svp::audio::build_asr_execution_boundary(
            chunk_plan, microphone_available, model_runtime_available,
            asr_model_available, asr_model_verified, microphone_plan.output_ref);
    svp::audio::AsrExecutionBoundary executed =
        svp::audio::execute_asr_boundary(
            boundary, staging_dir, model_cache_root,
            [progress, completed_chunks_before_stream, total_chunks](
                std::size_t current, std::size_t) {
              if (progress) {
                progress(completed_chunks_before_stream + current,
                         total_chunks);
              }
            });
    nlohmann::json stream_result = {
        {"source_audio_stream_id", microphone_plan.selected_source_audio_stream_id},
        {"source_stream_index", microphone_plan.source_stream_index},
        {"source_start_us", microphone_plan.source_start_us},
        {"timeline_duration_us", microphone_duration_us},
        {"analysis_audio_ref", microphone_plan.output_ref},
        {"asr_status", svp::audio::asr_status_to_string(executed.asr_status)},
        {"raw_word_count", executed.raw_word_count},
        {"reconciled_word_count", executed.reconciled_word_count},
        {"blockers", executed.blockers},
    };
    if (executed.asr_status == svp::audio::AsrStatus::ran) {
      svp::audio::MicrophoneTranscript transcript;
      transcript.source_audio_stream_id =
          microphone_plan.selected_source_audio_stream_id;
      transcript.source_ordinal = stream_ordinal;
      transcript.analysis_audio_ref = microphone_plan.output_ref;
      transcript.words = executed.reconciled_words;
      transcript.word_signal_db = svp::audio::measure_word_signal_db(
          staging_dir / microphone_plan.output_ref, transcript.words);
      transcripts.push_back(std::move(transcript));
    }
    result.stream_results.push_back(std::move(stream_result));
    boundaries.push_back(std::move(executed));
    completed_chunks_before_stream += chunks_by_stream[stream_ordinal];
  }
  svp::audio::release_whisper_cpp_model();

  std::size_t speech_positive_streams = 0;
  for (const auto& transcript : transcripts) {
    if (!transcript.words.empty()) ++speech_positive_streams;
  }

  // Whisper must finish every microphone before sherpa-onnx is loaded; loading
  // sherpa's bundled ONNX Runtime first can corrupt Whisper schema registration.
  const std::filesystem::path fingerprint_model_dir =
      model_cache_root / "model_sherpa_onnx_diarization";
  const bool fingerprint_runtime_available =
      speech_positive_streams > 1 &&
      std::filesystem::exists(fingerprint_model_dir) &&
      svp::audio::is_sherpa_diarization_available();

  std::vector<std::size_t> fingerprint_chunks_by_source(transcripts.size(), 0);
  std::size_t total_fingerprint_chunks = 0;
  if (fingerprint_runtime_available) {
    for (std::size_t index = 0; index < transcripts.size(); ++index) {
      const auto& transcript = transcripts[index];
      if (transcript.words.empty()) continue;
      try {
        fingerprint_chunks_by_source[index] =
            svp::audio::diarization_chunk_count(
                staging_dir / transcript.analysis_audio_ref);
        total_fingerprint_chunks += fingerprint_chunks_by_source[index];
      } catch (const std::exception&) {
        // The inference call below owns WAV-read failure reporting.
      }
    }
    if (diarization_progress.started) {
      diarization_progress.started();
    }
  }

  std::size_t completed_fingerprint_chunks = 0;
  for (std::size_t transcript_index = 0;
       transcript_index < transcripts.size(); ++transcript_index) {
    auto& transcript = transcripts[transcript_index];
    nlohmann::json& stream_result =
        result.stream_results[transcript.source_ordinal];
    if (transcript.words.empty()) {
      stream_result["voice_fingerprint"] = {
          {"status", "not_required_for_silent_stream"}, {"dimension", 0}};
      continue;
    }
    if (speech_positive_streams <= 1) {
      stream_result["voice_fingerprint"] = {
          {"status", "not_required_for_single_speech_stream"},
          {"dimension", 0}};
      continue;
    }
    if (fingerprint_runtime_available) {
      const svp::audio::SherpaDiarizationResult diarization =
          svp::audio::run_sherpa_diarization(
              staging_dir / transcript.analysis_audio_ref,
              fingerprint_model_dir, {},
              [diarization_progress, completed_fingerprint_chunks,
               total_fingerprint_chunks](std::size_t current, std::size_t) {
                if (diarization_progress.progress &&
                    total_fingerprint_chunks > 0) {
                  diarization_progress.progress(
                      completed_fingerprint_chunks + current,
                      total_fingerprint_chunks);
                }
              });
      transcript.voice_fingerprint =
          microphone_voice_fingerprint(diarization);
      transcript.voice_tracks = microphone_voice_tracks(diarization);
      transcript.signal_profile = svp::audio::measure_microphone_signal_profile(
          staging_dir / transcript.analysis_audio_ref,
          diarized_speech_spans(transcript.voice_tracks));
      stream_result["voice_fingerprint"] = {
          {"status", !transcript.voice_fingerprint.empty()
                         ? "available"
                         : "unavailable"},
          {"method", "duration_weighted_all_diarization_tracks"},
          {"dimension", transcript.voice_fingerprint.size()},
          {"diarization_speaker_count", diarization.final_speaker_count},
          {"time_aligned_voice_track_count", transcript.voice_tracks.size()},
          {"noise_floor_db",
           transcript.signal_profile.noise_floor_db.has_value()
               ? nlohmann::json(*transcript.signal_profile.noise_floor_db)
               : nlohmann::json(nullptr)},
          {"blockers", diarization.blockers},
      };
      completed_fingerprint_chunks +=
          fingerprint_chunks_by_source[transcript_index];
    } else {
      stream_result["voice_fingerprint"] = {
          {"status", "unavailable"},
          {"dimension", 0},
          {"blockers", {"speaker fingerprint runtime/model is unavailable"}},
      };
    }
    if (transcript.voice_fingerprint.empty()) {
      fingerprint_blockers.push_back(
          "microphone stream " + std::to_string(transcript.source_ordinal) +
          ": aggregate voice fingerprint is unavailable");
    }
  }

  if (boundaries.empty()) {
    result.boundary.asr_status = svp::audio::AsrStatus::blocked;
    result.boundary.blockers.push_back(
        "microphone ASR stage requires at least one microphone analysis stream");
    return result;
  }

  result.boundary = boundaries.front();
  result.boundary.input_refs.clear();
  result.boundary.staged_chunk_output_refs.clear();
  result.boundary.chunk_plan.chunks.clear();
  result.boundary.chunk_plan.total_duration_us = 0;
  result.boundary.blockers.clear();
  result.boundary.raw_word_count = 0;
  for (std::size_t stream_ordinal = 0; stream_ordinal < boundaries.size();
       ++stream_ordinal) {
    const auto& boundary = boundaries[stream_ordinal];
    result.boundary.input_refs.insert(result.boundary.input_refs.end(),
                                      boundary.input_refs.begin(),
                                      boundary.input_refs.end());
    result.boundary.raw_word_count += boundary.raw_word_count;
    result.boundary.chunk_plan.total_duration_us = std::max(
        result.boundary.chunk_plan.total_duration_us,
        boundary.chunk_plan.total_duration_us);
    for (const std::string& blocker : boundary.blockers) {
      result.boundary.blockers.push_back(
          "microphone stream " + std::to_string(stream_ordinal) + ": " + blocker);
    }
    for (auto chunk : boundary.chunk_plan.chunks) {
      std::ostringstream prefix;
      prefix << "microphone_" << std::setw(3) << std::setfill('0')
             << stream_ordinal << "_";
      chunk.chunk_id = prefix.str() + chunk.chunk_id;
      chunk.output_ref = "transcript/" + prefix.str() +
                         std::filesystem::path(chunk.output_ref).filename().string();
      result.boundary.staged_chunk_output_refs.push_back(chunk.output_ref);
      result.boundary.chunk_plan.chunks.push_back(std::move(chunk));
    }
  }
  if (!result.boundary.blockers.empty()) {
    result.boundary.asr_status = svp::audio::AsrStatus::blocked;
    result.boundary.reconciled_words.clear();
    result.boundary.reconciled_word_count = 0;
    return result;
  }

  const svp::audio::MicrophoneDeduplicationPolicy reconciliation_policy;
  const svp::audio::MicrophoneTranscriptResult reconciled =
      svp::audio::reconcile_microphone_transcripts(
          transcripts, reconciliation_policy);
  result.boundary.reconciled_words = reconciled.words;
  result.boundary.reconciled_word_count = reconciled.words.size();
  result.boundary.word_speaker_assignments = reconciled.word_speaker_assignments;
  result.boundary.speaker_segments = reconciled.speaker_segments;
  result.boundary.speaker_count = reconciled.speakers.size();
  result.boundary.speaker_source_audio_stream_ids.clear();
  result.boundary.speaker_source_audio_stream_groups.clear();
  for (const auto& speaker : reconciled.speakers) {
    result.boundary.speaker_source_audio_stream_ids.push_back(
        speaker.source_audio_stream_id);
    result.boundary.speaker_source_audio_stream_groups.push_back(
        speaker.source_audio_stream_ids);
  }
  result.boundary.one_speaker_mode = false;
  result.boundary.diarization_status = "microphone_stream_assignment";
  result.boundary.diarization_processor_id =
      "proc_microphone_stream_assignment_0001";
  result.boundary.diarization_note =
      "Each camera microphone remains an independent ownership source. Words are "
      "removed as bleed only when time-aligned turns share transcript content, "
      "local diarization fingerprints provide no strong contrary evidence, and another "
      "microphone has stronger time-local speech SNR. Missing fingerprint evidence "
      "preserves both sources. Decoder-residual words are removed only after sustained "
      "transcript, voice, shared-timing, and source-quality agreement; matching voice "
      "spans and stronger local SNR provide additional word-local support. Diarization was not "
      "allowed to reassign microphone ownership.";
  result.boundary.diarization_blockers = fingerprint_blockers;
  result.boundary.asr_status = svp::audio::AsrStatus::ran;

  result.reconciliation = {
      {"input_word_count", reconciled.input_word_count},
      {"duplicate_word_count", reconciled.duplicate_word_count},
      {"discarded_ambiguous_word_count",
       reconciled.discarded_ambiguous_word_count},
      {"discarded_cross_anchor_bleed_word_count",
       reconciled.discarded_cross_anchor_bleed_word_count},
      {"output_word_count", reconciled.words.size()},
      {"speaker_count", reconciled.speakers.size()},
      {"collapsed_microphone_stream_count",
       reconciled.collapsed_microphone_stream_count},
      {"identity_policy", "camera_microphone_stream_locked"},
      {"voice_match_policy",
       "time_aligned_diarization_fingerprints_are_supporting_word_local_evidence_only"},
      {"minimum_voice_fingerprint_similarity",
       reconciliation_policy.minimum_voice_fingerprint_similarity},
      {"maximum_contrary_voice_fingerprint_similarity",
       reconciliation_policy.maximum_contrary_voice_fingerprint_similarity},
      {"minimum_aligned_voice_coverage_ratio",
       reconciliation_policy.minimum_aligned_voice_coverage_ratio},
      {"minimum_aligned_voice_match_ratio",
       reconciliation_policy.minimum_aligned_voice_match_ratio},
      {"minimum_duplicate_chunk_token_alignment_ratio",
       reconciliation_policy.minimum_duplicate_chunk_token_alignment_ratio},
      {"minimum_duplicate_aligned_token_count",
       reconciliation_policy.minimum_duplicate_aligned_token_count},
      {"maximum_duplicate_word_time_delta_us",
       reconciliation_policy.maximum_duplicate_word_time_delta_us},
      {"maximum_residual_window_radius_us",
       reconciliation_policy.maximum_residual_window_radius_us},
      {"minimum_explained_duplicate_word_ratio",
       reconciliation_policy.minimum_explained_duplicate_word_ratio},
      {"minimum_explained_voice_ratio",
       reconciliation_policy.minimum_explained_voice_ratio},
      {"minimum_explained_shared_speech_ratio",
       reconciliation_policy.minimum_explained_shared_speech_ratio},
      {"primary_selection_policy",
       "stronger_time_local_snr_wins_only_after_local_content_and_fingerprint_agreement"},
      {"source_grouping_policy",
       "microphone_sources_remain_independent_unless_at_least_half_of_words_are_exact_duplicates_and_voice_timing_snr_and_asr_confidence_establish_bleed"},
      {"ranking_policy", "descending_deduplicated_word_count_then_stream_order"},
  };
  nlohmann::json voice_matches = nlohmann::json::array();
  for (const auto& evidence : reconciled.voice_match_evidence) {
    voice_matches.push_back({
        {"left_source_ordinal", evidence.left_source_ordinal},
        {"right_source_ordinal", evidence.right_source_ordinal},
        {"fingerprint_similarity", evidence.fingerprint_similarity},
        {"shared_speech_overlap_ratio",
         evidence.shared_speech_overlap_ratio},
        {"aligned_diarized_speech_us",
         evidence.aligned_diarized_speech_us},
        {"aligned_matching_voice_us",
         evidence.aligned_matching_voice_us},
        {"aligned_voice_coverage_ratio",
         evidence.aligned_voice_coverage_ratio},
        {"aligned_voice_match_ratio",
         evidence.aligned_voice_match_ratio},
        {"same_voice_proven", evidence.same_voice_proven},
    });
  }
  result.reconciliation["voice_match_evidence"] = std::move(voice_matches);
  nlohmann::json aligned_tracks = nlohmann::json::array();
  for (const auto& evidence : reconciled.aligned_track_evidence) {
    aligned_tracks.push_back({
        {"left_source_ordinal", evidence.left_source_ordinal},
        {"right_source_ordinal", evidence.right_source_ordinal},
        {"left_track_ordinal", evidence.left_track_ordinal},
        {"right_track_ordinal", evidence.right_track_ordinal},
        {"fingerprint_similarity", evidence.fingerprint_similarity},
        {"aligned_speech_us", evidence.aligned_speech_us},
        {"fingerprint_match", evidence.fingerprint_match},
    });
  }
  result.reconciliation["aligned_diarization_track_evidence"] =
      std::move(aligned_tracks);
  nlohmann::json source_quality = nlohmann::json::array();
  for (const auto& evidence : reconciled.source_quality_evidence) {
    source_quality.push_back({
        {"source_ordinal", evidence.source_ordinal},
        {"speech_coverage_us", evidence.speech_coverage_us},
        {"word_count", evidence.word_count},
        {"median_word_signal_db", evidence.median_word_signal_db},
        {"noise_floor_db",
         evidence.noise_floor_db.has_value()
             ? nlohmann::json(*evidence.noise_floor_db)
             : nlohmann::json(nullptr)},
        {"median_speech_snr_db",
         evidence.median_speech_snr_db.has_value()
             ? nlohmann::json(*evidence.median_speech_snr_db)
             : nlohmann::json(nullptr)},
        {"mean_asr_confidence", evidence.mean_asr_confidence},
    });
  }
  result.reconciliation["source_quality_evidence"] =
      std::move(source_quality);
  nlohmann::json source_assignments = nlohmann::json::array();
  for (const auto& evidence : reconciled.source_assignment_evidence) {
    source_assignments.push_back({
        {"source_ordinal", evidence.source_ordinal},
        {"decision", evidence.decision},
        {"anchor_source_ordinal",
         evidence.anchor_source_ordinal.has_value()
             ? nlohmann::json(*evidence.anchor_source_ordinal)
             : nlohmann::json(nullptr)},
        {"matched_stronger_source_ordinals",
         evidence.matched_stronger_source_ordinals},
    });
  }
  result.reconciliation["source_assignment_evidence"] =
      std::move(source_assignments);
  nlohmann::json chunk_content = nlohmann::json::array();
  for (const auto& evidence : reconciled.chunk_content_evidence) {
    chunk_content.push_back({
        {"left_source_ordinal", evidence.left_source_ordinal},
        {"right_source_ordinal", evidence.right_source_ordinal},
        {"chunk_ordinal", evidence.chunk_ordinal},
        {"left_token_count", evidence.left_token_count},
        {"right_token_count", evidence.right_token_count},
        {"aligned_token_count", evidence.aligned_token_count},
        {"token_alignment_ratio", evidence.token_alignment_ratio},
        {"time_aligned", evidence.time_aligned},
        {"local_fingerprint_not_contrary",
         evidence.local_fingerprint_not_contrary},
        {"duplicate_capture_proven", evidence.duplicate_capture_proven},
    });
  }
  result.reconciliation["cross_anchor_chunk_content_evidence"] =
      std::move(chunk_content);
  nlohmann::json speaker_sources = nlohmann::json::array();
  for (const auto& speaker : reconciled.speakers) {
    speaker_sources.push_back({
        {"speaker_id", speaker.speaker_id},
        {"source_audio_stream_id", speaker.source_audio_stream_id},
        {"source_ordinal", speaker.source_ordinal},
        {"word_count", speaker.word_count},
        {"source_audio_stream_ids", speaker.source_audio_stream_ids},
    });
  }
  result.reconciliation["speakers"] = std::move(speaker_sources);
  nlohmann::json input_streams = nlohmann::json::array();
  for (const auto& microphone : extraction_plan.microphone_analysis_streams) {
    input_streams.push_back({
        {"source_audio_stream_id",
         microphone.selected_source_audio_stream_id},
        {"source_stream_index", microphone.source_stream_index},
        {"source_start_us", microphone.source_start_us},
        {"timeline_duration_us",
         microphone.timeline_duration_us.has_value()
             ? nlohmann::json(*microphone.timeline_duration_us)
             : nlohmann::json(nullptr)},
        {"input_ref", microphone.output_ref},
    });
  }
  nlohmann::json fingerprint_models = nlohmann::json::array();
  if (fingerprint_runtime_available) {
    fingerprint_models.push_back("model_sherpa_onnx_diarization");
  }
  result.processor_record = {
      {"id", result.boundary.diarization_processor_id},
      {"name", "SVP microphone stream reconciliation"},
      {"version", "1"},
      {"input_refs", result.boundary.input_refs},
      {"output_refs",
       {result.boundary.transcript_output_ref, result.boundary.words_output_ref,
        result.boundary.speakers_output_ref,
        result.boundary.chunk_provenance_ref}},
      {"model_refs", fingerprint_models},
      {"runtime", "svp-audio"},
      {"execution_provider", "cpu"},
      {"identity_policy", "camera_microphone_stream_locked"},
      {"input_streams", input_streams},
      {"supporting_fingerprint_blockers", fingerprint_blockers},
      {"reconciliation", result.reconciliation},
      {"completed", true},
  };
  return result;
}

}  // namespace svp::builder
