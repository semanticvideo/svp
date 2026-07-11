#pragma once

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/transcript_records.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

// Camera microphone inputs are authoritative evidence sources. Inputs collapse
// to one speaker only when voice identity and shared speech timing both prove
// that one input is carrying bleed from the same person.
struct MicrophoneDeduplicationPolicy {
  // Matches the existing diarization fingerprint threshold. Fingerprint match
  // alone is insufficient; shared speech timing below must also pass.
  double minimum_voice_fingerprint_similarity = 0.60;
  // Similarity at or below this bound is strong contrary identity evidence.
  // Values between this and the positive-match threshold are inconclusive and
  // must be combined with local transcript and SNR evidence.
  double maximum_contrary_voice_fingerprint_similarity = 0.20;
  // A matching voice must cover most of the smaller microphone's diarized
  // speech. This prevents a short incidental bleed match from merging inputs.
  double minimum_aligned_voice_coverage_ratio = 0.60;
  // Most time where both microphones contain diarized speech must agree on the
  // voice fingerprint. A mismatch at the same time is contrary identity evidence.
  double minimum_aligned_voice_match_ratio = 0.60;
  // Time-local turns/windows require a strict majority of their combined
  // normalized token mass to align. Whole ASR chunks are never treated as
  // duplicate captures.
  double minimum_duplicate_chunk_token_alignment_ratio = 0.50;
  // At least two aligned words are required so a common isolated token cannot
  // delete legitimate simultaneous speech.
  std::size_t minimum_duplicate_aligned_token_count = 2;
  // Whisper timestamps are segment-derived rather than force-aligned. This
  // tolerance permits small decoder timing drift while still requiring local,
  // not chunk-wide, agreement between microphone captures.
  std::int64_t maximum_duplicate_word_time_delta_us = 1500000;
  // Residual ASR tokens use a symmetric window twice the word-pair tolerance
  // so differently segmented decoders can still establish local agreement.
  std::int64_t maximum_residual_window_radius_us = 3000000;
  // Source-level bleed classification requires at least half of the words to
  // have already passed exact word-local duplicate proof. This is only an
  // initial content gate: voice, timing, SNR, and ASR-confidence gates below
  // must independently agree before decoder residue can be removed.
  double minimum_explained_duplicate_word_ratio = 0.50;
  // Nearly all of the weaker source's voice and simultaneous speech must match
  // before unmatched ASR residue can be evaluated as bleed.
  double minimum_explained_voice_ratio = 0.95;
  double minimum_explained_shared_speech_ratio = 0.95;
  // Words from one microphone separated by at most 750 ms remain one speaker
  // segment, matching the transcript writer's existing utterance-gap policy.
  std::int64_t maximum_speaker_segment_gap_us = 750000;
};

struct MicrophoneSignalPolicy {
  // 20 ms is short enough to follow speech/noise changes while retaining enough
  // PCM samples for a stable RMS measurement at 16 kHz.
  std::int64_t frame_duration_us = 20000;
  // The median of diarization-negative frames is robust to occasional unmarked
  // transients while representing the channel's ordinary non-speech level.
  double noise_floor_percentile = 0.50;
};

struct MicrophoneVoiceTrack {
  std::size_t track_ordinal = 0;
  std::vector<TimeSpan> speech_segments;
  std::vector<float> fingerprint;
};

struct MicrophoneSignalFrame {
  TimeSpan timing;
  double signal_db = -120.0;
};

struct MicrophoneSignalProfile {
  std::optional<double> noise_floor_db;
  std::vector<MicrophoneSignalFrame> frames;
};

struct MicrophoneTranscript {
  std::string source_audio_stream_id;
  std::size_t source_ordinal = 0;
  std::string analysis_audio_ref;
  std::vector<AsrWord> words;
  std::vector<double> word_signal_db;
  std::vector<float> voice_fingerprint;
  std::vector<MicrophoneVoiceTrack> voice_tracks;
  MicrophoneSignalProfile signal_profile;
};

struct MicrophoneSpeakerSummary {
  std::string source_audio_stream_id;
  std::size_t source_ordinal = 0;
  std::string speaker_id;
  std::size_t word_count = 0;
  std::vector<std::string> source_audio_stream_ids;
};

struct MicrophoneVoiceMatchEvidence {
  std::size_t left_source_ordinal = 0;
  std::size_t right_source_ordinal = 0;
  double fingerprint_similarity = -1.0;
  double shared_speech_overlap_ratio = 0.0;
  std::int64_t aligned_diarized_speech_us = 0;
  std::int64_t aligned_matching_voice_us = 0;
  double aligned_voice_coverage_ratio = 0.0;
  double aligned_voice_match_ratio = 0.0;
  bool same_voice_proven = false;
};

struct MicrophoneAlignedTrackEvidence {
  std::size_t left_source_ordinal = 0;
  std::size_t right_source_ordinal = 0;
  std::size_t left_track_ordinal = 0;
  std::size_t right_track_ordinal = 0;
  double fingerprint_similarity = -1.0;
  std::int64_t aligned_speech_us = 0;
  bool fingerprint_match = false;
};

struct MicrophoneSourceQualityEvidence {
  std::size_t source_ordinal = 0;
  std::int64_t speech_coverage_us = 0;
  std::size_t word_count = 0;
  double median_word_signal_db = -120.0;
  std::optional<double> noise_floor_db;
  std::optional<double> median_speech_snr_db;
  double mean_asr_confidence = 0.0;
};

struct MicrophoneSourceAssignmentEvidence {
  std::size_t source_ordinal = 0;
  std::string decision;
  std::optional<std::size_t> anchor_source_ordinal;
  std::vector<std::size_t> matched_stronger_source_ordinals;
};

struct MicrophoneOwnedWordCandidate {
  AsrWord word;
  std::size_t voice_group = 0;
  std::size_t source_ordinal = 0;
};

struct MicrophoneSpeakerAnchor {
  std::size_t voice_group = 0;
  std::size_t transcript_index = 0;
};

struct MicrophoneChunkContentEvidence {
  std::size_t left_source_ordinal = 0;
  std::size_t right_source_ordinal = 0;
  std::int64_t chunk_ordinal = 0;
  std::size_t left_token_count = 0;
  std::size_t right_token_count = 0;
  std::size_t aligned_token_count = 0;
  double token_alignment_ratio = 0.0;
  bool time_aligned = false;
  bool local_fingerprint_not_contrary = false;
  bool duplicate_capture_proven = false;
};

struct MicrophoneWordOwnershipResult {
  std::vector<MicrophoneOwnedWordCandidate> words;
  std::vector<MicrophoneChunkContentEvidence> chunk_content_evidence;
  std::size_t discarded_cross_anchor_bleed_word_count = 0;
  std::map<std::size_t, std::size_t> discarded_word_count_by_source;
  std::map<std::size_t, std::map<std::size_t, std::size_t>>
      discarded_word_count_by_source_pair;
};

struct MicrophoneTranscriptResult {
  std::vector<AsrWord> words;
  std::vector<std::string> word_speaker_assignments;
  std::vector<SpeakerSegment> speaker_segments;
  std::vector<MicrophoneSpeakerSummary> speakers;
  std::vector<MicrophoneVoiceMatchEvidence> voice_match_evidence;
  std::vector<MicrophoneAlignedTrackEvidence> aligned_track_evidence;
  std::vector<MicrophoneSourceQualityEvidence> source_quality_evidence;
  std::vector<MicrophoneSourceAssignmentEvidence> source_assignment_evidence;
  std::vector<MicrophoneChunkContentEvidence> chunk_content_evidence;
  std::size_t input_word_count = 0;
  std::size_t duplicate_word_count = 0;
  std::size_t discarded_ambiguous_word_count = 0;
  std::size_t discarded_cross_anchor_bleed_word_count = 0;
  std::size_t collapsed_microphone_stream_count = 0;
  std::map<std::size_t, std::map<std::size_t, std::size_t>>
      discarded_word_count_by_source_pair;
};

[[nodiscard]] std::vector<double> measure_word_signal_db(
    const std::filesystem::path& wav_path,
    const std::vector<AsrWord>& words);

[[nodiscard]] MicrophoneSignalProfile measure_microphone_signal_profile(
    const std::filesystem::path& wav_path,
    const std::vector<TimeSpan>& diarized_speech,
    const MicrophoneSignalPolicy& policy = {});

[[nodiscard]] MicrophoneTranscriptResult reconcile_microphone_transcripts(
    const std::vector<MicrophoneTranscript>& transcripts,
    const MicrophoneDeduplicationPolicy& policy = {});

[[nodiscard]] MicrophoneWordOwnershipResult
reconcile_cross_anchor_word_ownership(
    const std::vector<MicrophoneOwnedWordCandidate>& words,
    const std::vector<MicrophoneTranscript>& transcripts,
    const std::vector<MicrophoneSpeakerAnchor>& anchors,
    const MicrophoneDeduplicationPolicy& policy = {});

}  // namespace svp::audio
