#include "svp/audio/transcript_writer.hpp"
#include "svp/audio/transcript_records.hpp"

#include "svp/media/canonical_timing.hpp"

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
    throw std::runtime_error("unable to open transcript JSON: " + path.string());
  }
  output << value.dump(2) << "\n";
}

void write_jsonl_file(const std::filesystem::path& path,
                      const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to open transcript JSONL: " + path.string());
  }
  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

void write_empty_jsonl(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to open transcript JSONL: " + path.string());
  }
}

std::string asr_status_string(AsrStatus status) {
  switch (status) {
    case AsrStatus::planned: return "planned";
    case AsrStatus::blocked: return "blocked";
    case AsrStatus::ran: return "ran";
  }
  return "unknown";
}

std::string word_id_for_ordinal(std::size_t ordinal) {
  std::ostringstream output;
  output << "word_" << std::setw(6) << std::setfill('0') << ordinal;
  return output.str();
}

nlohmann::json diarization_provenance_json(const AsrExecutionBoundary& boundary) {
  std::string note;
  if (boundary.diarization_status == "ran") {
    note = "Diarization model ran and produced speaker segments.";
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
      {"note", note},
  };
  if (!boundary.diarization_blockers.empty()) {
    result["blockers"] = boundary.diarization_blockers;
  }
  return result;
}

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
      {"confidence_status", "unimplemented"},
      {"confidence_note", "Per-word confidence is not yet extracted from decoder logits. All confidence values are 0.0."},
      {"speaker_mode", boundary.diarization_status == "fallback_one_speaker"
           ? "one_speaker_fallback"
           : "diarization_assigned"},
      {"speaker_note", boundary.diarization_status == "fallback_one_speaker"
           ? "Single speaker assigned without diarization. All words have speaker_id speaker_0001. This is fallback behavior, not speaker recognition."
           : "Speaker IDs assigned by diarization processor."},
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
                                     const std::string& asr_status) {
  nlohmann::json asr_limitations = {
      {"timestamp_method", "whisper_timestamp_token_segments"},
      {"timestamp_precision", "words_distributed_evenly_within_segment"},
      {"confidence_status", "unimplemented"},
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

nlohmann::json speaker_json(const std::string& speaker_id,
                            const std::string& processor_id,
                            const std::string& diarization_status) {
  return {
      {"id", speaker_id},
      {"display_name", "Speaker 1"},
      {"total_speech_us", 0},
      {"confidence", 0.0},
      {"processor_id", processor_id},
      {"diarization_status", diarization_status},
  };
}

}  // namespace

TranscriptWriteResult write_transcript_artifacts(const AsrExecutionBoundary& boundary,
                                                  const std::filesystem::path& staging_root) {
  TranscriptWriteResult result;

  const std::filesystem::path transcript_path = staging_root / boundary.transcript_output_ref;
  const std::filesystem::path words_path = staging_root / boundary.words_output_ref;
  const std::filesystem::path speakers_path = staging_root / boundary.speakers_output_ref;
  const std::filesystem::path provenance_path = staging_root / boundary.chunk_provenance_ref;
  const std::filesystem::path speech_regions_path =
      staging_root / "transcript" / "speech_regions.jsonl";
  const std::filesystem::path speaker_segments_path =
      staging_root / "transcript" / "speaker_segments.jsonl";

  const bool blocked = (boundary.asr_status != AsrStatus::ran);

  if (blocked) {
    write_json_file(transcript_path, blocked_transcript_json(boundary));
    result.transcript_written = true;

    write_empty_jsonl(words_path);
    result.words_written = true;
    result.word_count = 0;

    write_empty_jsonl(speakers_path);
    result.speakers_written = true;
    result.speaker_count = 0;

    result.transcript_status = asr_status_string(boundary.asr_status);
    result.blockers = boundary.blockers;
  } else {
    write_json_file(transcript_path,
                    ran_transcript_json(boundary,
                                        boundary.reconciled_word_count,
                                        boundary.speaker_count));
    result.transcript_written = true;

    std::vector<nlohmann::json> word_records;
    const std::string speaker_id = "speaker_0001";
    for (std::size_t i = 0; i < boundary.reconciled_words.size(); ++i) {
      const AsrWord& w = boundary.reconciled_words[i];
      word_records.push_back({
          {"id", word_id_for_ordinal(i)},
          {"text", w.text},
          {"start_us", w.start_us},
          {"end_us", w.end_us},
          {"confidence", w.confidence},
          {"chunk_ordinal", w.chunk_ordinal},
          {"speaker_id", speaker_id},
      });
    }
    write_jsonl_file(words_path, word_records);
    result.words_written = true;
    result.word_count = boundary.reconciled_word_count;

    if (boundary.one_speaker_mode) {
      write_jsonl_file(speakers_path,
                       {speaker_json("speaker_0001", boundary.diarization_processor_id,
                                     boundary.diarization_status)});
      result.speakers_written = true;
      result.speaker_count = 1;
    } else {
      write_empty_jsonl(speakers_path);
      result.speakers_written = true;
      result.speaker_count = boundary.speaker_count;
    }

    result.transcript_status = "ran";
  }

  std::vector<nlohmann::json> provenance_records;
  for (const AsrChunkPlan& chunk : boundary.chunk_plan.chunks) {
    provenance_records.push_back(
        chunk_provenance_json(chunk, boundary.processor_id,
                              asr_status_string(boundary.asr_status)));
  }
  write_jsonl_file(provenance_path, provenance_records);
  result.chunk_provenance_written = true;

  if (!std::filesystem::exists(speech_regions_path)) {
    write_empty_jsonl(speech_regions_path);
  }
  result.speech_regions_written = true;

  if (blocked) {
    write_empty_jsonl(speaker_segments_path);
  } else if (!boundary.speaker_segments.empty()) {
    std::vector<nlohmann::json> segment_records;
    for (const SpeakerSegment& seg : boundary.speaker_segments) {
      segment_records.push_back(speaker_segment_to_json(seg));
    }
    write_jsonl_file(speaker_segments_path, segment_records);
  } else if (!std::filesystem::exists(speaker_segments_path)) {
    write_empty_jsonl(speaker_segments_path);
  }
  result.speaker_segments_written = true;

  return result;
}

nlohmann::json transcript_write_result_to_json(const TranscriptWriteResult& result) {
  return {
      {"transcript_written", result.transcript_written},
      {"words_written", result.words_written},
      {"speakers_written", result.speakers_written},
      {"chunk_provenance_written", result.chunk_provenance_written},
      {"speech_regions_written", result.speech_regions_written},
      {"speaker_segments_written", result.speaker_segments_written},
      {"word_count", result.word_count},
      {"speaker_count", result.speaker_count},
      {"speech_region_count", result.speech_region_count},
      {"transcript_status", result.transcript_status},
      {"blockers", result.blockers},
  };
}

}  // namespace svp::audio
