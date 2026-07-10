#include "internal.hpp"

#include <algorithm>
#include <set>

namespace svp::audio::transcript_writer_internal {
namespace {

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
                            std::int64_t total_speech_us,
                            const std::string& source_audio_stream_id = "") {
  std::string display_name = "Speaker " + std::to_string(speaker_number);
  nlohmann::json speaker = {
      {"id", speaker_id},
      {"display_name", display_name},
      {"total_speech_us", total_speech_us},
      {"confidence", 0.0},
      {"processor_id", processor_id},
      {"diarization_status", diarization_status},
  };
  if (!source_audio_stream_id.empty()) {
    speaker["source_audio_stream_id"] = source_audio_stream_id;
  }
  return speaker;
}

}  // namespace

std::vector<nlohmann::json> build_speaker_records(
    const AsrExecutionBoundary& boundary,
    const std::map<std::string, std::vector<TimeSpan>>& speaker_intervals) {
  if (boundary.one_speaker_mode) {
    std::int64_t total_speech = 0;
    auto it = speaker_intervals.find("speaker_0001");
    if (it != speaker_intervals.end()) {
      total_speech = compute_total_speech_us(it->second);
    }
    return {speaker_json("speaker_0001", boundary.diarization_processor_id,
                         boundary.diarization_status, 1, total_speech)};
  }

  std::set<std::string> unique_speaker_ids;
  for (const SpeakerSegment& seg : boundary.speaker_segments) {
    unique_speaker_ids.insert(seg.speaker_id);
  }
  for (const auto& [sid, intervals] : speaker_intervals) {
    (void)intervals;
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
    const std::string source_audio_stream_id =
        boundary.diarization_status == "microphone_stream_assignment" &&
                static_cast<std::size_t>(speaker_number) <=
                    boundary.speaker_source_audio_stream_ids.size()
            ? boundary.speaker_source_audio_stream_ids[
                  static_cast<std::size_t>(speaker_number - 1)]
            : "";
    speaker_records.push_back(speaker_json(
        sid, boundary.diarization_processor_id, boundary.diarization_status,
        speaker_number, total_speech, source_audio_stream_id));
    speaker_number++;
  }
  return speaker_records;
}

}  // namespace svp::audio::transcript_writer_internal
