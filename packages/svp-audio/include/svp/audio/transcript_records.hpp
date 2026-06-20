#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

struct TimeSpan {
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
};

struct SpeechRegion {
  std::string id;
  TimeSpan timing;
  double confidence = 0.0;
  std::string processor_id;
};

struct LanguageSummary {
  std::string primary = "und";
  std::vector<std::string> detected;
  std::string mode = "undetermined";
  double confidence = 0.0;
};

struct TranscriptSummary {
  LanguageSummary language;
  std::int64_t duration_us = 0;
  std::int64_t word_count = 0;
  std::int64_t speaker_count = 0;
  std::string source_audio_id;
  std::string processor_id;
};

struct SpeakerCandidate {
  std::string speaker_id;
  double confidence = 0.0;
};

struct TranscriptWord {
  std::string id;
  std::string text;
  std::string normalized_text;
  TimeSpan timing;
  std::string speaker_id;
  std::string speech_region_id;
  double confidence = 0.0;
  std::optional<bool> speech_overlap;
  std::vector<SpeakerCandidate> speaker_candidates;
  std::optional<std::string> attached_to_word_id;
};

struct Speaker {
  std::string id;
  std::string display_name;
  std::optional<std::string> embedding_ref;
  std::int64_t total_speech_us = 0;
  double confidence = 0.0;
  std::string processor_id;
};

struct SpeakerSegment {
  std::string id;
  std::string speaker_id;
  TimeSpan timing;
  double confidence = 0.0;
  bool overlap = false;
};

[[nodiscard]] nlohmann::json time_span_to_json(TimeSpan value);
[[nodiscard]] nlohmann::json speech_region_to_json(const SpeechRegion& region);
[[nodiscard]] nlohmann::json language_summary_to_json(const LanguageSummary& language);
[[nodiscard]] nlohmann::json transcript_summary_to_json(const TranscriptSummary& summary);
[[nodiscard]] nlohmann::json speaker_candidate_to_json(const SpeakerCandidate& candidate);
[[nodiscard]] nlohmann::json transcript_word_to_json(const TranscriptWord& word);
[[nodiscard]] nlohmann::json speaker_to_json(const Speaker& speaker);
[[nodiscard]] nlohmann::json speaker_segment_to_json(const SpeakerSegment& segment);

}  // namespace svp::audio
