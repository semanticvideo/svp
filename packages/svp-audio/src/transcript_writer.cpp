#include "svp/audio/transcript_writer.hpp"
#include "svp/audio/transcript_records.hpp"

#include "transcript_writer/internal.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
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

}  // namespace

TranscriptWriteResult write_transcript_artifacts(
    const AsrExecutionBoundary& boundary,
    const std::filesystem::path& staging_root) {
  namespace writer = transcript_writer_internal;

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
    write_json_file(transcript_path, writer::blocked_transcript_json(boundary));
    result.transcript_written = true;

    write_empty_jsonl(words_path);
    result.words_written = true;
    result.word_count = 0;

    write_empty_jsonl(speakers_path);
    result.speakers_written = true;
    result.speaker_count = 0;

    result.transcript_status = writer::asr_status_string(boundary.asr_status);
    result.blockers = boundary.blockers;
  } else {
    write_json_file(transcript_path,
                    writer::ran_transcript_json(boundary,
                                                boundary.reconciled_word_count,
                                                boundary.speaker_count));
    result.transcript_written = true;

    const std::vector<std::string> word_speaker_assignments =
        writer::assign_word_speakers(boundary);
    const writer::WordRecordBuildResult word_records =
        writer::build_word_records(boundary.reconciled_words,
                                   word_speaker_assignments);
    write_jsonl_file(words_path, word_records.records);
    result.words_written = true;
    result.word_count = boundary.reconciled_word_count;

    const std::vector<nlohmann::json> speaker_records =
        writer::build_speaker_records(boundary, word_records.speaker_intervals);
    write_jsonl_file(speakers_path, speaker_records);
    result.speakers_written = true;
    result.speaker_count = speaker_records.size();

    result.transcript_status = "ran";
  }

  std::vector<nlohmann::json> provenance_records;
  for (const AsrChunkPlan& chunk : boundary.chunk_plan.chunks) {
    provenance_records.push_back(
        writer::chunk_provenance_json(chunk, boundary.processor_id,
                                      writer::asr_status_string(boundary.asr_status),
                                      boundary.diarization_status));
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
