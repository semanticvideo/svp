#include "svp/audio/transcript_records.hpp"

#include "svp/media/canonical_timing.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace svp::audio {
namespace {

void require_valid_span(TimeSpan value) {
  if (value.start_us < 0 || value.end_us < 0) {
    throw std::invalid_argument("audio transcript timing must be non-negative");
  }
  if (value.end_us <= value.start_us) {
    throw std::invalid_argument("audio transcript span end_us must be greater than start_us");
  }
}

nlohmann::json unchecked_time_span_to_json(TimeSpan value) {
  return {
      {"start_us", value.start_us},
      {"end_us", value.end_us},
      {"start_sec", svp::media::microseconds_to_seconds_string(value.start_us)},
      {"end_sec", svp::media::microseconds_to_seconds_string(value.end_us)},
  };
}

}  // namespace

nlohmann::json time_span_to_json(TimeSpan value) {
  require_valid_span(value);
  return unchecked_time_span_to_json(value);
}

nlohmann::json speech_region_to_json(const SpeechRegion& region) {
  nlohmann::json value = time_span_to_json(region.timing);
  value["id"] = region.id;
  value["confidence"] = region.confidence;
  value["processor_id"] = region.processor_id;
  return value;
}

nlohmann::json language_summary_to_json(const LanguageSummary& language) {
  return {
      {"primary", language.primary},
      {"detected", language.detected},
      {"mode", language.mode},
      {"confidence", language.confidence},
  };
}

nlohmann::json transcript_summary_to_json(const TranscriptSummary& summary) {
  return {
      {"language", language_summary_to_json(summary.language)},
      {"duration_us", summary.duration_us},
      {"word_count", summary.word_count},
      {"speaker_count", summary.speaker_count},
      {"source_audio_id", summary.source_audio_id},
      {"processor_id", summary.processor_id},
  };
}

nlohmann::json speaker_candidate_to_json(const SpeakerCandidate& candidate) {
  return {
      {"speaker_id", candidate.speaker_id},
      {"confidence", candidate.confidence},
  };
}

nlohmann::json transcript_word_to_json(const TranscriptWord& word) {
  if (word.timing.start_us < 0 || word.timing.end_us < 0) {
    throw std::invalid_argument("audio transcript timing must be non-negative");
  }
  if (word.timing.end_us < word.timing.start_us) {
    throw std::invalid_argument("audio transcript word end_us must be greater than start_us");
  }
  if (word.timing.end_us == word.timing.start_us && !word.attached_to_word_id.has_value()) {
    throw std::invalid_argument("zero-duration transcript words must be attached to a word");
  }

  nlohmann::json value = unchecked_time_span_to_json(word.timing);
  value["id"] = word.id;
  value["text"] = word.text;
  value["normalized_text"] = word.normalized_text;
  value["speaker_id"] = word.speaker_id;
  value["speech_region_id"] = word.speech_region_id;
  value["confidence"] = word.confidence;

  if (word.speech_overlap.has_value()) {
    value["speech_overlap"] = *word.speech_overlap;
  }
  if (!word.speaker_candidates.empty()) {
    value["speaker_candidates"] = nlohmann::json::array();
    for (const SpeakerCandidate& candidate : word.speaker_candidates) {
      value["speaker_candidates"].push_back(speaker_candidate_to_json(candidate));
    }
  }
  if (word.attached_to_word_id.has_value()) {
    value["attached_to_word_id"] = *word.attached_to_word_id;
  }

  return value;
}

nlohmann::json speaker_to_json(const Speaker& speaker) {
  nlohmann::json value = {
      {"id", speaker.id},
      {"display_name", speaker.display_name},
      {"total_speech_us", speaker.total_speech_us},
      {"confidence", speaker.confidence},
      {"processor_id", speaker.processor_id},
  };
  if (speaker.embedding_ref.has_value()) {
    value["embedding_ref"] = *speaker.embedding_ref;
  }
  return value;
}

nlohmann::json speaker_segment_to_json(const SpeakerSegment& segment) {
  nlohmann::json value = time_span_to_json(segment.timing);
  value["id"] = segment.id;
  value["speaker_id"] = segment.speaker_id;
  value["confidence"] = segment.confidence;
  value["overlap"] = segment.overlap;
  return value;
}

}  // namespace svp::audio
