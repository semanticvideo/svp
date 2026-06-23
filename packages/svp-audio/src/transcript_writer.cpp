#include "svp/audio/transcript_writer.hpp"
#include "svp/audio/transcript_records.hpp"

#include "svp/media/canonical_timing.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace svp::audio {
namespace {

constexpr std::int64_t speaker_assignment_tolerance_us = 500000;

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
      {"confidence_status", "decoder_token_softmax_mean"},
      {"confidence_note", "Per-word confidence is the mean of selected-token decoder softmax probabilities for the word's constituent tokens. This is uncalibrated model confidence, not a calibrated probability."},
      {"speaker_mode", boundary.diarization_status == "fallback_one_speaker"
           ? "one_speaker_fallback"
           : "diarization_assigned"},
      {"speaker_note", boundary.diarization_status == "fallback_one_speaker"
           ? "Single speaker assigned without diarization. All words have speaker_id speaker_0001. This is fallback behavior, not speaker recognition."
           : "Speaker IDs assigned by max interval overlap with nearest-segment fallback (500ms tolerance). Words outside all segments and tolerance are marked speaker_unknown."},
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
      {"confidence_status", "decoder_token_softmax_mean"},
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

std::int64_t compute_total_speech_us(const std::vector<TimeSpan>& intervals) {
  if (intervals.empty()) {
    return 0;
  }
  std::vector<TimeSpan> sorted = intervals;
  std::sort(sorted.begin(), sorted.end(),
            [](const TimeSpan& a, const TimeSpan& b) {
              return a.start_us < b.start_us;
            });
  std::int64_t total = 0;
  std::int64_t merge_start = sorted[0].start_us;
  std::int64_t merge_end = sorted[0].end_us;
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i].start_us <= merge_end) {
      if (sorted[i].end_us > merge_end) {
        merge_end = sorted[i].end_us;
      }
    } else {
      total += merge_end - merge_start;
      merge_start = sorted[i].start_us;
      merge_end = sorted[i].end_us;
    }
  }
  total += merge_end - merge_start;
  return total;
}

nlohmann::json speaker_json(const std::string& speaker_id,
                            const std::string& processor_id,
                            const std::string& diarization_status,
                            int speaker_number,
                            std::int64_t total_speech_us) {
  std::string display_name = "Speaker " + std::to_string(speaker_number);
  return {
      {"id", speaker_id},
      {"display_name", display_name},
      {"total_speech_us", total_speech_us},
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
    std::map<std::string, std::vector<TimeSpan>> speaker_intervals;
    for (std::size_t i = 0; i < boundary.reconciled_words.size(); ++i) {
      const AsrWord& w = boundary.reconciled_words[i];
      std::string word_speaker_id = "speaker_unknown";
      if (boundary.one_speaker_mode) {
        word_speaker_id = "speaker_0001";
      } else if (!boundary.speaker_segments.empty()) {
        const SpeakerSegment* best_seg = nullptr;
        std::int64_t best_overlap = 0;
        for (const SpeakerSegment& seg : boundary.speaker_segments) {
          std::int64_t overlap = std::min(w.end_us, seg.timing.end_us) -
                                 std::max(w.start_us, seg.timing.start_us);
          if (overlap > best_overlap) {
            best_overlap = overlap;
            best_seg = &seg;
          }
        }
        if (best_seg) {
          word_speaker_id = best_seg->speaker_id;
        } else {
          const SpeakerSegment* nearest_seg = nullptr;
          std::int64_t nearest_dist = std::numeric_limits<std::int64_t>::max();
          for (const SpeakerSegment& seg : boundary.speaker_segments) {
            std::int64_t dist;
            if (w.end_us <= seg.timing.start_us) {
              dist = seg.timing.start_us - w.end_us;
            } else if (w.start_us >= seg.timing.end_us) {
              dist = w.start_us - seg.timing.end_us;
            } else {
              dist = 0;
            }
            if (dist < nearest_dist) {
              nearest_dist = dist;
              nearest_seg = &seg;
            }
          }
          if (nearest_seg && nearest_dist <= speaker_assignment_tolerance_us) {
            word_speaker_id = nearest_seg->speaker_id;
          }
        }
      }
      speaker_intervals[word_speaker_id].push_back({w.start_us, w.end_us});
      word_records.push_back({
          {"id", word_id_for_ordinal(i)},
          {"text", w.text},
          {"start_us", w.start_us},
          {"end_us", w.end_us},
          {"confidence", w.confidence},
          {"chunk_ordinal", w.chunk_ordinal},
          {"speaker_id", word_speaker_id},
      });
    }
    write_jsonl_file(words_path, word_records);
    result.words_written = true;
    result.word_count = boundary.reconciled_word_count;

    if (boundary.one_speaker_mode) {
      std::int64_t total_speech = 0;
      auto it = speaker_intervals.find("speaker_0001");
      if (it != speaker_intervals.end()) {
        total_speech = compute_total_speech_us(it->second);
      }
      write_jsonl_file(speakers_path,
                       {speaker_json("speaker_0001", boundary.diarization_processor_id,
                                     boundary.diarization_status, 1, total_speech)});
      result.speakers_written = true;
      result.speaker_count = 1;
    } else {
      std::set<std::string> unique_speaker_ids;
      for (const SpeakerSegment& seg : boundary.speaker_segments) {
        unique_speaker_ids.insert(seg.speaker_id);
      }
      for (const auto& [sid, intervals] : speaker_intervals) {
        unique_speaker_ids.insert(sid);
      }
      std::vector<nlohmann::json> speaker_records;
      int speaker_number = 1;
      for (const std::string& sid : unique_speaker_ids) {
        std::int64_t total_speech = 0;
        auto it = speaker_intervals.find(sid);
        if (it != speaker_intervals.end()) {
          total_speech = compute_total_speech_us(it->second);
        }
        speaker_records.push_back(speaker_json(sid, boundary.diarization_processor_id,
                                               boundary.diarization_status, speaker_number,
                                               total_speech));
        speaker_number++;
      }
      write_jsonl_file(speakers_path, speaker_records);
      result.speakers_written = true;
      result.speaker_count = unique_speaker_ids.size();
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
