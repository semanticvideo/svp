#include "svp/audio/microphone_transcript.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>

namespace svp::audio {
namespace {

constexpr double kMinimumSignalDb = -120.0;

std::vector<TimeSpan> speech_spans(
    const std::vector<AsrWord>& words,
    std::int64_t maximum_gap_us) {
  std::vector<TimeSpan> spans;
  for (const AsrWord& word : words) {
    if (word.end_us > word.start_us) {
      spans.push_back({word.start_us, word.end_us});
    }
  }
  std::sort(spans.begin(), spans.end(), [](const TimeSpan& a, const TimeSpan& b) {
    return a.start_us < b.start_us;
  });
  std::vector<TimeSpan> merged;
  for (const TimeSpan& span : spans) {
    if (merged.empty() ||
        span.start_us - merged.back().end_us > maximum_gap_us) {
      merged.push_back(span);
    } else {
      merged.back().end_us = std::max(merged.back().end_us, span.end_us);
    }
  }
  return merged;
}

std::vector<TimeSpan> merge_spans(std::vector<TimeSpan> spans) {
  std::sort(spans.begin(), spans.end(), [](const TimeSpan& left,
                                           const TimeSpan& right) {
    return left.start_us < right.start_us;
  });
  std::vector<TimeSpan> merged;
  for (const TimeSpan& span : spans) {
    if (span.end_us <= span.start_us) continue;
    if (merged.empty() || span.start_us > merged.back().end_us) {
      merged.push_back(span);
    } else {
      merged.back().end_us = std::max(merged.back().end_us, span.end_us);
    }
  }
  return merged;
}

std::vector<TimeSpan> voice_speech_spans(
    const MicrophoneTranscript& transcript) {
  std::vector<TimeSpan> spans;
  for (const auto& track : transcript.voice_tracks) {
    spans.insert(spans.end(), track.speech_segments.begin(),
                 track.speech_segments.end());
  }
  return merge_spans(std::move(spans));
}

std::vector<TimeSpan> intersect_spans(const std::vector<TimeSpan>& left,
                                      const std::vector<TimeSpan>& right) {
  std::vector<TimeSpan> intersections;
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.size() && right_index < right.size()) {
    const std::int64_t start =
        std::max(left[left_index].start_us, right[right_index].start_us);
    const std::int64_t end =
        std::min(left[left_index].end_us, right[right_index].end_us);
    if (end > start) intersections.push_back({start, end});
    if (left[left_index].end_us <= right[right_index].end_us) {
      ++left_index;
    } else {
      ++right_index;
    }
  }
  return intersections;
}

std::int64_t total_duration(const std::vector<TimeSpan>& spans) {
  std::int64_t total = 0;
  for (const TimeSpan& span : spans) {
    total += std::max<std::int64_t>(0, span.end_us - span.start_us);
  }
  return total;
}

std::int64_t intersection_duration(const std::vector<TimeSpan>& left,
                                   const std::vector<TimeSpan>& right) {
  std::int64_t total = 0;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() && j < right.size()) {
    total += std::max<std::int64_t>(
        0, std::min(left[i].end_us, right[j].end_us) -
               std::max(left[i].start_us, right[j].start_us));
    if (left[i].end_us <= right[j].end_us) {
      ++i;
    } else {
      ++j;
    }
  }
  return total;
}

double fingerprint_similarity(const std::vector<float>& left,
                              const std::vector<float>& right) {
  if (left.empty() || left.size() != right.size()) return -1.0;
  double dot = 0.0;
  double left_norm = 0.0;
  double right_norm = 0.0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    dot += static_cast<double>(left[i]) * static_cast<double>(right[i]);
    left_norm += static_cast<double>(left[i]) * static_cast<double>(left[i]);
    right_norm += static_cast<double>(right[i]) * static_cast<double>(right[i]);
  }
  if (left_norm <= 0.0 || right_norm <= 0.0) return -1.0;
  return dot / (std::sqrt(left_norm) * std::sqrt(right_norm));
}

double shared_speech_ratio(const MicrophoneTranscript& left,
                           const MicrophoneTranscript& right,
                           const MicrophoneDeduplicationPolicy& policy) {
  const std::vector<TimeSpan> left_spans =
      speech_spans(left.words, policy.maximum_speaker_segment_gap_us);
  const std::vector<TimeSpan> right_spans =
      speech_spans(right.words, policy.maximum_speaker_segment_gap_us);
  const std::int64_t smaller_duration =
      std::min(total_duration(left_spans), total_duration(right_spans));
  if (smaller_duration <= 0) return 0.0;
  return static_cast<double>(intersection_duration(left_spans, right_spans)) /
         static_cast<double>(smaller_duration);
}

double mean_confidence(const MicrophoneTranscript& transcript) {
  if (transcript.words.empty()) return 0.0;
  double total = 0.0;
  for (const AsrWord& word : transcript.words) total += word.confidence;
  return total / static_cast<double>(transcript.words.size());
}

double median_signal_db(const MicrophoneTranscript& transcript) {
  std::vector<double> finite_signals;
  for (const double signal : transcript.word_signal_db) {
    if (std::isfinite(signal)) finite_signals.push_back(signal);
  }
  if (finite_signals.empty()) return -std::numeric_limits<double>::infinity();
  std::sort(finite_signals.begin(), finite_signals.end());
  const std::size_t middle = finite_signals.size() / 2;
  if ((finite_signals.size() & 1U) != 0U) return finite_signals[middle];
  return (finite_signals[middle - 1] + finite_signals[middle]) / 2.0;
}

std::optional<double> median_speech_snr_db(
    const MicrophoneTranscript& transcript) {
  if (!transcript.signal_profile.noise_floor_db.has_value()) return std::nullopt;
  const double signal_db = median_signal_db(transcript);
  if (!std::isfinite(signal_db)) return std::nullopt;
  return signal_db - *transcript.signal_profile.noise_floor_db;
}

bool better_primary(const MicrophoneTranscript& candidate,
                    const MicrophoneTranscript& current,
                    const MicrophoneDeduplicationPolicy& policy) {
  const double candidate_signal = median_signal_db(candidate);
  const double current_signal = median_signal_db(current);
  const std::optional<double> candidate_snr = median_speech_snr_db(candidate);
  const std::optional<double> current_snr = median_speech_snr_db(current);
  if (candidate_snr.has_value() && current_snr.has_value() &&
      *candidate_snr != *current_snr) {
    return *candidate_snr > *current_snr;
  }
  if (candidate_signal != current_signal) return candidate_signal > current_signal;
  const std::int64_t candidate_coverage = total_duration(
      speech_spans(candidate.words, policy.maximum_speaker_segment_gap_us));
  const std::int64_t current_coverage = total_duration(
      speech_spans(current.words, policy.maximum_speaker_segment_gap_us));
  if (candidate_coverage != current_coverage) {
    return candidate_coverage > current_coverage;
  }
  if (candidate.words.size() != current.words.size()) {
    return candidate.words.size() > current.words.size();
  }
  const double candidate_confidence = mean_confidence(candidate);
  const double current_confidence = mean_confidence(current);
  if (candidate_confidence != current_confidence) {
    return candidate_confidence > current_confidence;
  }
  return candidate.source_ordinal < current.source_ordinal;
}

bool overlaps_covered_speech(const AsrWord& word,
                             const std::vector<TimeSpan>& coverage) {
  for (const TimeSpan& span : coverage) {
    if (span.start_us >= word.end_us) break;
    if (std::min(span.end_us, word.end_us) >
        std::max(span.start_us, word.start_us)) {
      return true;
    }
  }
  return false;
}

std::string speaker_id_for_rank(std::size_t rank) {
  std::ostringstream output;
  output << "speaker_" << std::setw(4) << std::setfill('0') << rank + 1;
  return output.str();
}

std::vector<SpeakerSegment> build_segments(
    const std::vector<AsrWord>& words,
    const std::vector<std::string>& assignments,
    const MicrophoneDeduplicationPolicy& policy) {
  std::map<std::string, std::vector<TimeSpan>> spans_by_speaker;
  for (std::size_t i = 0; i < words.size(); ++i) {
    spans_by_speaker[assignments[i]].push_back({words[i].start_us, words[i].end_us});
  }

  std::vector<SpeakerSegment> segments;
  for (auto& [speaker_id, spans] : spans_by_speaker) {
    std::sort(spans.begin(), spans.end(), [](const TimeSpan& a, const TimeSpan& b) {
      return a.start_us < b.start_us;
    });
    if (spans.empty()) continue;
    TimeSpan current = spans.front();
    for (std::size_t i = 1; i < spans.size(); ++i) {
      if (spans[i].start_us - current.end_us <=
          policy.maximum_speaker_segment_gap_us) {
        current.end_us = std::max(current.end_us, spans[i].end_us);
      } else {
        segments.push_back({"", speaker_id, current, 1.0, false});
        current = spans[i];
      }
    }
    segments.push_back({"", speaker_id, current, 1.0, false});
  }

  std::sort(segments.begin(), segments.end(), [](const SpeakerSegment& a,
                                                  const SpeakerSegment& b) {
    if (a.timing.start_us != b.timing.start_us) {
      return a.timing.start_us < b.timing.start_us;
    }
    return a.speaker_id < b.speaker_id;
  });
  for (std::size_t i = 0; i < segments.size(); ++i) {
    std::ostringstream id;
    id << "speakerseg_" << std::setw(6) << std::setfill('0') << i;
    segments[i].id = id.str();
    for (std::size_t j = 0; j < segments.size(); ++j) {
      if (i == j || segments[i].speaker_id == segments[j].speaker_id) continue;
      if (std::min(segments[i].timing.end_us, segments[j].timing.end_us) >
          std::max(segments[i].timing.start_us, segments[j].timing.start_us)) {
        segments[i].overlap = true;
        break;
      }
    }
  }
  return segments;
}

}  // namespace

MicrophoneTranscriptResult reconcile_microphone_transcripts(
    const std::vector<MicrophoneTranscript>& transcripts,
    const MicrophoneDeduplicationPolicy& policy) {
  MicrophoneTranscriptResult result;
  for (const auto& transcript : transcripts) {
    const std::optional<double> speech_snr = median_speech_snr_db(transcript);
    result.input_word_count += transcript.words.size();
    result.source_quality_evidence.push_back({
        transcript.source_ordinal,
        total_duration(speech_spans(
            transcript.words, policy.maximum_speaker_segment_gap_us)),
        transcript.words.size(),
        std::isfinite(median_signal_db(transcript))
            ? median_signal_db(transcript)
            : kMinimumSignalDb,
        transcript.signal_profile.noise_floor_db,
        speech_snr,
        mean_confidence(transcript),
    });
  }

  std::vector<std::vector<bool>> same_voice_proven(
      transcripts.size(), std::vector<bool>(transcripts.size(), false));
  for (std::size_t i = 0; i < transcripts.size(); ++i) {
    same_voice_proven[i][i] = true;
  }
  for (std::size_t i = 0; i < transcripts.size(); ++i) {
    if (transcripts[i].words.empty()) continue;
    for (std::size_t j = i + 1; j < transcripts.size(); ++j) {
      if (transcripts[j].words.empty()) continue;
      const double similarity = fingerprint_similarity(
          transcripts[i].voice_fingerprint,
          transcripts[j].voice_fingerprint);
      const double overlap =
          shared_speech_ratio(transcripts[i], transcripts[j], policy);

      std::vector<TimeSpan> all_aligned_spans;
      std::vector<TimeSpan> matching_aligned_spans;
      for (const auto& left_track : transcripts[i].voice_tracks) {
        const std::vector<TimeSpan> left_segments =
            merge_spans(left_track.speech_segments);
        for (const auto& right_track : transcripts[j].voice_tracks) {
          const std::vector<TimeSpan> right_segments =
              merge_spans(right_track.speech_segments);
          const std::vector<TimeSpan> aligned =
              intersect_spans(left_segments, right_segments);
          const std::int64_t aligned_us = total_duration(aligned);
          if (aligned_us <= 0) continue;
          const double track_similarity = fingerprint_similarity(
              left_track.fingerprint, right_track.fingerprint);
          const bool fingerprint_match =
              track_similarity >= policy.minimum_voice_fingerprint_similarity;
          result.aligned_track_evidence.push_back({
              transcripts[i].source_ordinal,
              transcripts[j].source_ordinal,
              left_track.track_ordinal,
              right_track.track_ordinal,
              track_similarity,
              aligned_us,
              fingerprint_match,
          });
          all_aligned_spans.insert(all_aligned_spans.end(), aligned.begin(),
                                   aligned.end());
          if (fingerprint_match) {
            matching_aligned_spans.insert(matching_aligned_spans.end(),
                                          aligned.begin(), aligned.end());
          }
        }
      }
      all_aligned_spans = merge_spans(std::move(all_aligned_spans));
      matching_aligned_spans =
          merge_spans(std::move(matching_aligned_spans));
      const std::int64_t aligned_speech_us =
          total_duration(all_aligned_spans);
      const std::int64_t matching_voice_us =
          total_duration(matching_aligned_spans);
      const std::int64_t smaller_voice_coverage_us = std::min(
          total_duration(voice_speech_spans(transcripts[i])),
          total_duration(voice_speech_spans(transcripts[j])));
      const double aligned_voice_coverage_ratio =
          smaller_voice_coverage_us > 0
              ? static_cast<double>(matching_voice_us) /
                    static_cast<double>(smaller_voice_coverage_us)
              : 0.0;
      const double aligned_voice_match_ratio =
          aligned_speech_us > 0
              ? static_cast<double>(matching_voice_us) /
                    static_cast<double>(aligned_speech_us)
              : 0.0;
      const bool same_voice =
          aligned_voice_coverage_ratio >=
              policy.minimum_aligned_voice_coverage_ratio &&
          aligned_voice_match_ratio >=
              policy.minimum_aligned_voice_match_ratio;
      result.voice_match_evidence.push_back(
          {transcripts[i].source_ordinal, transcripts[j].source_ordinal,
           similarity, overlap, aligned_speech_us, matching_voice_us,
           aligned_voice_coverage_ratio, aligned_voice_match_ratio,
           same_voice});
      same_voice_proven[i][j] = same_voice;
      same_voice_proven[j][i] = same_voice;
    }
  }

  std::vector<std::size_t> quality_order;
  for (std::size_t index = 0; index < transcripts.size(); ++index) {
    if (!transcripts[index].words.empty()) quality_order.push_back(index);
  }
  std::sort(quality_order.begin(), quality_order.end(),
            [&](std::size_t left, std::size_t right) {
              if (left == right) return false;
              return better_primary(transcripts[left], transcripts[right],
                                    policy);
            });

  std::vector<std::optional<std::size_t>> assigned_group(transcripts.size());
  std::map<std::size_t, std::vector<std::size_t>> members_by_group;
  std::map<std::size_t, std::size_t> anchor_by_group;
  std::size_t next_group = 0;
  for (std::size_t order = 0; order < quality_order.size(); ++order) {
    const std::size_t source = quality_order[order];
    std::vector<std::size_t> matched_stronger_sources;
    std::vector<std::size_t> matched_groups;
    for (std::size_t stronger_order = 0; stronger_order < order;
         ++stronger_order) {
      const std::size_t stronger_source = quality_order[stronger_order];
      if (!same_voice_proven[source][stronger_source] ||
          !assigned_group[stronger_source].has_value()) {
        continue;
      }
      matched_stronger_sources.push_back(
          transcripts[stronger_source].source_ordinal);
      const std::size_t group = *assigned_group[stronger_source];
      if (std::find(matched_groups.begin(), matched_groups.end(), group) ==
          matched_groups.end()) {
        matched_groups.push_back(group);
      }
    }

    MicrophoneSourceAssignmentEvidence assignment;
    assignment.source_ordinal = transcripts[source].source_ordinal;
    assignment.matched_stronger_source_ordinals = matched_stronger_sources;
    if (matched_groups.empty()) {
      const std::size_t group = next_group++;
      assigned_group[source] = group;
      members_by_group[group].push_back(source);
      anchor_by_group[group] = source;
      assignment.decision = "speaker_anchor";
      assignment.anchor_source_ordinal = transcripts[source].source_ordinal;
    } else if (matched_groups.size() == 1) {
      const std::size_t group = matched_groups.front();
      assigned_group[source] = group;
      members_by_group[group].push_back(source);
      assignment.decision = "attached_to_stronger_source";
      assignment.anchor_source_ordinal =
          transcripts[anchor_by_group.at(group)].source_ordinal;
      ++result.collapsed_microphone_stream_count;
    } else {
      assignment.decision = "discarded_ambiguous_across_speaker_groups";
      result.discarded_ambiguous_word_count += transcripts[source].words.size();
    }
    result.source_assignment_evidence.push_back(std::move(assignment));
  }

  std::vector<MicrophoneOwnedWordCandidate> candidates;
  std::map<std::size_t, std::size_t> primary_by_group;
  for (auto& [group, members] : members_by_group) {
    std::size_t primary = members.front();
    for (const std::size_t member : members) {
      if (better_primary(transcripts[member], transcripts[primary], policy)) {
        primary = member;
      }
    }
    primary_by_group[group] = primary;
    std::sort(members.begin(), members.end(), [&](std::size_t left,
                                                   std::size_t right) {
      if (left == right) return false;
      if (left == primary) return true;
      if (right == primary) return false;
      return better_primary(transcripts[left], transcripts[right], policy);
    });

    std::vector<TimeSpan> covered;
    for (const std::size_t member : members) {
      for (const AsrWord& word : transcripts[member].words) {
        if (member == primary || !overlaps_covered_speech(word, covered)) {
          candidates.push_back({word, group,
                                transcripts[member].source_ordinal});
        }
      }
      std::vector<AsrWord> accepted_words;
      for (const MicrophoneOwnedWordCandidate& candidate : candidates) {
        if (candidate.voice_group == group) {
          accepted_words.push_back(candidate.word);
        }
      }
      covered = speech_spans(accepted_words,
                             policy.maximum_speaker_segment_gap_us);
    }
  }

  std::vector<MicrophoneSpeakerAnchor> anchors;
  for (const auto& [group, primary] : primary_by_group) {
    anchors.push_back({group, primary});
  }
  MicrophoneWordOwnershipResult ownership =
      reconcile_cross_anchor_word_ownership(candidates, transcripts, anchors,
                                            policy);
  candidates = std::move(ownership.words);
  result.chunk_content_evidence =
      std::move(ownership.chunk_content_evidence);
  result.discarded_cross_anchor_bleed_word_count =
      ownership.discarded_cross_anchor_bleed_word_count;

  std::map<std::size_t, std::size_t> word_counts;
  for (const MicrophoneOwnedWordCandidate& candidate : candidates) {
    ++word_counts[candidate.voice_group];
  }
  result.duplicate_word_count =
      result.input_word_count - result.discarded_ambiguous_word_count -
      result.discarded_cross_anchor_bleed_word_count - candidates.size();

  for (const auto& [group, members] : members_by_group) {
    const auto found = word_counts.find(group);
    if (found == word_counts.end() || found->second == 0) continue;
    const std::size_t primary = primary_by_group.at(group);
    MicrophoneSpeakerSummary summary;
    summary.source_audio_stream_id =
        transcripts[primary].source_audio_stream_id;
    summary.source_ordinal = transcripts[primary].source_ordinal;
    summary.word_count = found->second;
    for (const std::size_t member : members) {
      summary.source_audio_stream_ids.push_back(
          transcripts[member].source_audio_stream_id);
    }
    result.speakers.push_back(std::move(summary));
  }
  std::sort(result.speakers.begin(), result.speakers.end(),
            [](const MicrophoneSpeakerSummary& a,
               const MicrophoneSpeakerSummary& b) {
              if (a.word_count != b.word_count) return a.word_count > b.word_count;
              return a.source_ordinal < b.source_ordinal;
            });
  std::unordered_map<std::size_t, std::string> speaker_ids_by_group;
  for (std::size_t rank = 0; rank < result.speakers.size(); ++rank) {
    result.speakers[rank].speaker_id = speaker_id_for_rank(rank);
    for (const auto& [group, primary] : primary_by_group) {
      if (transcripts[primary].source_ordinal ==
          result.speakers[rank].source_ordinal) {
        speaker_ids_by_group[group] = result.speakers[rank].speaker_id;
        break;
      }
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const MicrophoneOwnedWordCandidate& a,
               const MicrophoneOwnedWordCandidate& b) {
    if (a.word.start_us != b.word.start_us) {
      return a.word.start_us < b.word.start_us;
    }
    if (a.word.end_us != b.word.end_us) {
      return a.word.end_us < b.word.end_us;
    }
    return a.source_ordinal < b.source_ordinal;
  });
  for (const MicrophoneOwnedWordCandidate& candidate : candidates) {
    result.words.push_back(candidate.word);
    result.word_speaker_assignments.push_back(
        speaker_ids_by_group.at(candidate.voice_group));
  }
  result.speaker_segments =
      build_segments(result.words, result.word_speaker_assignments, policy);
  return result;
}

}  // namespace svp::audio
