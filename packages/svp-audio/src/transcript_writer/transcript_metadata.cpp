#include "internal.hpp"

namespace svp::audio::transcript_writer_internal {
namespace {

nlohmann::json diarization_provenance_json(const AsrExecutionBoundary& boundary) {
  std::string note;
  if (boundary.diarization_status == "ran") {
    if (boundary.speaker_count == 0 || boundary.speaker_segments.empty()) {
      note = "Diarization model ran and detected no speech or speaker segments.";
    } else {
      note = "Diarization model ran and produced speaker segments.";
    }
  } else if (boundary.diarization_status == "user_declared_single_speaker") {
    note = boundary.diarization_note.empty()
         ? "User requested single-speaker mode; diarization was intentionally skipped."
         : boundary.diarization_note;
  } else if (boundary.diarization_status == "fallback_one_speaker") {
    note = boundary.diarization_note.empty()
         ? "One-speaker fallback used. This is not speaker recognition."
         : boundary.diarization_note;
  } else {
    note = boundary.diarization_note.empty()
         ? "Diarization did not run."
         : boundary.diarization_note;
  }

  nlohmann::json result = {
      {"status", boundary.diarization_status},
      {"processor_id", boundary.diarization_processor_id},
      {"one_speaker_fallback", boundary.diarization_status == "fallback_one_speaker"},
      {"user_declared_single_speaker", boundary.diarization_status == "user_declared_single_speaker"},
      {"note", note},
  };
  if (!boundary.diarization_blockers.empty()) {
    result["blockers"] = boundary.diarization_blockers;
  }
  return result;
}

}  // namespace

nlohmann::json blocked_transcript_json(const AsrExecutionBoundary& boundary) {
  nlohmann::json language = {
      {"primary", "und"},
      {"detected", nlohmann::json::array()},
      {"mode", "undetermined"},
      {"confidence", 0.0},
  };

  return {
      {"language", language},
      {"duration_us", boundary.chunk_plan.total_duration_us},
      {"word_count", 0},
      {"speaker_count", 0},
      {"source_audio_id", "astream_analysis_0001"},
      {"processor_id", boundary.processor_id},
      {"asr_status", asr_status_string(boundary.asr_status)},
      {"one_speaker_mode", boundary.one_speaker_mode},
      {"diarization", diarization_provenance_json(boundary)},
      {"blockers", boundary.blockers},
  };
}

nlohmann::json ran_transcript_json(const AsrExecutionBoundary& boundary,
                                   std::size_t word_count,
                                   std::size_t speaker_count) {
  nlohmann::json language = {
      {"primary", "en"},
      {"detected", {"en"}},
      {"mode", "single"},
      {"confidence", 0.0},
  };

  nlohmann::json asr_limitations = {
      {"timestamp_method", "whisper_timestamp_token_segments"},
      {"timestamp_precision", "words_distributed_evenly_within_segment"},
      {"timestamp_note", "Word start_us/end_us are derived from Whisper decoder timestamp tokens (50357+). Words are distributed evenly within each timestamp segment, not cross-attention aligned."},
      {"confidence_status", "decoder_token_softmax_mean"},
      {"confidence_note", "Per-word confidence is the mean of selected-token decoder softmax probabilities for the word's constituent tokens. This is uncalibrated model confidence, not a calibrated probability."},
      {"speaker_mode", boundary.diarization_status == "fallback_one_speaker"
           ? "one_speaker_fallback"
           : (boundary.diarization_status == "user_declared_single_speaker"
                ? "user_declared_single_speaker"
                : "diarization_assigned")},
      {"speaker_note", boundary.diarization_status == "fallback_one_speaker"
           ? "Single speaker assigned without diarization. All words have speaker_id speaker_0001. This is fallback behavior, not speaker recognition."
           : (boundary.diarization_status == "user_declared_single_speaker"
                ? "User requested single-speaker mode. All words have speaker_id speaker_0001. Diarization was intentionally skipped."
                : "Speaker IDs assigned by max interval overlap with nearest-segment fallback (500ms tolerance). Sustained non-dominant speaker evidence may be expanded across the current ASR utterance. Words outside all segments and tolerance are marked speaker_unknown.")},
  };

  return {
      {"language", language},
      {"duration_us", boundary.chunk_plan.total_duration_us},
      {"word_count", word_count},
      {"speaker_count", speaker_count},
      {"source_audio_id", "astream_analysis_0001"},
      {"processor_id", boundary.processor_id},
      {"asr_status", asr_status_string(boundary.asr_status)},
      {"one_speaker_mode", boundary.one_speaker_mode},
      {"diarization", diarization_provenance_json(boundary)},
      {"asr_limitations", asr_limitations},
  };
}

nlohmann::json chunk_provenance_json(const AsrChunkPlan& chunk,
                                     const std::string& processor_id,
                                     const std::string& asr_status,
                                     const std::string& diarization_status) {
  std::string speaker_mode = "diarization_assigned";
  if (diarization_status == "fallback_one_speaker") {
    speaker_mode = "one_speaker_fallback";
  } else if (diarization_status == "user_declared_single_speaker") {
    speaker_mode = "user_declared_single_speaker";
  }
  nlohmann::json asr_limitations = {
      {"timestamp_method", "whisper_timestamp_token_segments"},
      {"timestamp_precision", "words_distributed_evenly_within_segment"},
      {"confidence_status", "decoder_token_softmax_mean"},
      {"speaker_mode", speaker_mode},
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

}  // namespace svp::audio::transcript_writer_internal
