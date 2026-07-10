#include "audio_test_support.hpp"

#include "svp/audio/microphone_transcript.hpp"

namespace {

svp::audio::AsrWord word(std::string text, std::int64_t start_us,
                         std::int64_t end_us, double confidence = 0.8) {
  return {std::move(text), start_us, end_us, confidence, 0};
}

svp::audio::MicrophoneVoiceTrack voice_track(
    std::vector<float> fingerprint,
    std::int64_t start_us,
    std::int64_t end_us,
    std::size_t ordinal = 0) {
  return {ordinal, {{start_us, end_us}}, std::move(fingerprint)};
}

}  // namespace

void test_microphone_bleed_deduplication_and_word_count_ranking() {
  svp::audio::MicrophoneTranscript microphone_0;
  microphone_0.source_audio_stream_id = "astream_0001";
  microphone_0.source_ordinal = 0;
  microphone_0.words = {
      word("Hello", 1000000, 1400000, 0.90),
      word("alpha", 1500000, 1900000, 0.90),
      word("extra", 6000000, 6400000, 0.90),
  };
  microphone_0.word_signal_db = {-10.0, -12.0, -11.0};
  microphone_0.voice_fingerprint = {1.0f, 0.0f, 0.0f};
  microphone_0.voice_tracks = {
      voice_track({1.0f, 0.0f, 0.0f}, 1000000, 6400000)};
  microphone_0.signal_profile.noise_floor_db = -50.0;
  microphone_0.signal_profile.frames = {{{1000000, 1900000}, -10.0}};

  svp::audio::MicrophoneTranscript microphone_1;
  microphone_1.source_audio_stream_id = "astream_0002";
  microphone_1.source_ordinal = 1;
  microphone_1.words = {
      word("hello", 1050000, 1450000, 0.80),
      word("alpha", 1500000, 1900000, 0.90),
  };
  microphone_1.word_signal_db = {-24.0, -24.0};
  microphone_1.voice_fingerprint = {0.99f, 0.01f, 0.0f};
  microphone_1.voice_tracks = {
      voice_track({0.99f, 0.01f, 0.0f}, 1050000, 1900000)};
  microphone_1.signal_profile.noise_floor_db = -42.0;
  microphone_1.signal_profile.frames = {{{1050000, 1900000}, -24.0}};

  const auto result = svp::audio::reconcile_microphone_transcripts(
      {microphone_0, microphone_1});

  assert(result.input_word_count == 5);
  assert(result.duplicate_word_count == 0);
  assert(result.discarded_cross_anchor_bleed_word_count == 2);
  assert(result.words.size() == 3);
  assert(result.speakers.size() == 1);
  assert(result.collapsed_microphone_stream_count == 1);
  assert(result.speakers[0].source_audio_stream_id == "astream_0001");
  assert(result.speakers[0].speaker_id == "speaker_0001");
  assert(result.speakers[0].word_count == 3);
  assert(result.speakers[0].source_audio_stream_ids.size() == 2);
  assert(result.speakers[0].source_audio_stream_ids[1] == "astream_0002");
  assert(result.words.front().text == "Hello");
  assert(std::all_of(result.word_speaker_assignments.begin(),
                     result.word_speaker_assignments.end(),
                     [](const std::string& id) { return id == "speaker_0001"; }));
  assert(result.voice_match_evidence.size() == 1);
  assert(result.voice_match_evidence[0].same_voice_proven);
  assert(result.source_quality_evidence.size() == 2);
}

void test_microphone_equal_evidence_preserves_simultaneous_words() {
  svp::audio::MicrophoneTranscript microphone_0;
  microphone_0.source_audio_stream_id = "astream_0001";
  microphone_0.source_ordinal = 0;
  microphone_0.words = {word("yes", 1000000, 1300000, 0.85)};
  microphone_0.word_signal_db = {-12.0};
  microphone_0.voice_fingerprint = {1.0f, 0.0f};
  microphone_0.voice_tracks = {
      voice_track({1.0f, 0.0f}, 1000000, 1300000)};

  svp::audio::MicrophoneTranscript microphone_1;
  microphone_1.source_audio_stream_id = "astream_0002";
  microphone_1.source_ordinal = 1;
  microphone_1.words = {word("yes", 1020000, 1320000, 0.84)};
  microphone_1.word_signal_db = {-12.5};
  microphone_1.voice_fingerprint = {0.0f, 1.0f};
  microphone_1.voice_tracks = {
      voice_track({0.0f, 1.0f}, 1020000, 1320000)};

  const auto result = svp::audio::reconcile_microphone_transcripts(
      {microphone_0, microphone_1});

  assert(result.duplicate_word_count == 0);
  assert(result.words.size() == 2);
  assert(result.speakers.size() == 2);
  assert(!result.voice_match_evidence[0].same_voice_proven);
}

void test_microphone_matching_voice_without_shared_timing_stays_separate() {
  svp::audio::MicrophoneTranscript microphone_0;
  microphone_0.source_audio_stream_id = "astream_0001";
  microphone_0.source_ordinal = 0;
  microphone_0.words = {word("early", 1000000, 1300000)};
  microphone_0.word_signal_db = {-10.0};
  microphone_0.voice_fingerprint = {1.0f, 0.0f};
  microphone_0.voice_tracks = {
      voice_track({1.0f, 0.0f}, 1000000, 1300000)};

  svp::audio::MicrophoneTranscript microphone_1;
  microphone_1.source_audio_stream_id = "astream_0002";
  microphone_1.source_ordinal = 1;
  microphone_1.words = {word("late", 10000000, 10300000)};
  microphone_1.word_signal_db = {-20.0};
  microphone_1.voice_fingerprint = {0.99f, 0.01f};
  microphone_1.voice_tracks = {
      voice_track({0.99f, 0.01f}, 10000000, 10300000)};

  const auto result = svp::audio::reconcile_microphone_transcripts(
      {microphone_0, microphone_1});

  assert(result.speakers.size() == 2);
  assert(result.collapsed_microphone_stream_count == 0);
  assert(result.voice_match_evidence.size() == 1);
  assert(!result.voice_match_evidence[0].same_voice_proven);
  assert(result.voice_match_evidence[0].shared_speech_overlap_ratio == 0.0);
}

void test_microphone_primary_selection_prefers_acoustic_strength() {
  svp::audio::MicrophoneTranscript strong;
  strong.source_audio_stream_id = "astream_0001";
  strong.source_ordinal = 0;
  strong.words = {word("complete", 1000000, 2000000, 0.80)};
  strong.word_signal_db = {-8.0};
  strong.voice_fingerprint = {1.0f, 0.0f};
  strong.voice_tracks = {
      voice_track({1.0f, 0.0f}, 1000000, 2000000)};
  strong.signal_profile.noise_floor_db = -50.0;

  svp::audio::MicrophoneTranscript weak;
  weak.source_audio_stream_id = "astream_0002";
  weak.source_ordinal = 1;
  weak.words = {
      word("com", 1000000, 1300000, 0.95),
      word("ple", 1300000, 1600000, 0.95),
      word("te", 1600000, 2000000, 0.95),
  };
  weak.word_signal_db = {-22.0, -21.0, -20.0};
  weak.voice_fingerprint = {0.99f, 0.01f};
  weak.voice_tracks = {
      voice_track({0.99f, 0.01f}, 1000000, 2000000)};
  weak.signal_profile.noise_floor_db = -35.0;

  const auto result =
      svp::audio::reconcile_microphone_transcripts({strong, weak});

  assert(result.speakers.size() == 2);
  assert(result.speakers[0].source_audio_stream_id == "astream_0002");
  assert(result.speakers[0].word_count == 3);
  assert(result.words.size() == 4);
  assert(std::any_of(result.words.begin(), result.words.end(),
                     [](const svp::audio::AsrWord& candidate) {
                       return candidate.text == "complete";
                     }));
  assert(result.duplicate_word_count == 0);
  assert(result.source_quality_evidence[0].median_speech_snr_db == 42.0);
  assert(result.source_quality_evidence[1].median_speech_snr_db == 14.0);
}

void test_time_aligned_diarization_preserves_two_speakers_and_collapses_bleed() {
  svp::audio::MicrophoneTranscript direct_a;
  direct_a.source_audio_stream_id = "astream_0002";
  direct_a.source_ordinal = 1;
  direct_a.words = {
      word("speaker", 1000000, 1400000),
      word("a", 1400000, 1800000),
      word("keeps", 1800000, 2200000),
      word("talking", 2200000, 2600000),
  };
  direct_a.word_signal_db = {-15.0, -14.0, -16.0, -15.0};
  direct_a.voice_fingerprint = {1.0f, 0.0f, 0.0f};
  direct_a.voice_tracks = {
      voice_track({1.0f, 0.0f, 0.0f}, 1000000, 2600000)};
  direct_a.signal_profile.noise_floor_db = -55.0;

  svp::audio::MicrophoneTranscript direct_b;
  direct_b.source_audio_stream_id = "astream_0003";
  direct_b.source_ordinal = 2;
  direct_b.words = {
      word("speaker", 1050000, 1500000),
      word("b", 1500000, 1950000),
      word("answers", 1950000, 2500000),
  };
  direct_b.word_signal_db = {-13.0, -14.0, -13.0};
  direct_b.voice_fingerprint = {0.0f, 1.0f, 0.0f};
  direct_b.voice_tracks = {
      voice_track({0.0f, 1.0f, 0.0f}, 1050000, 2500000)};
  direct_b.signal_profile.noise_floor_db = -52.0;

  svp::audio::MicrophoneTranscript bleed_a;
  bleed_a.source_audio_stream_id = "astream_0004";
  bleed_a.source_ordinal = 3;
  bleed_a.words = {
      word("speaker", 1020000, 1420000),
      word("a", 1420000, 1820000),
      word("keeps", 1820000, 2220000),
      word("talking", 2220000, 2620000),
  };
  bleed_a.word_signal_db = {-31.0, -30.0, -32.0, -31.0};
  bleed_a.voice_fingerprint = {0.99f, 0.01f, 0.0f};
  bleed_a.voice_tracks = {
      voice_track({0.99f, 0.01f, 0.0f}, 1020000, 2300000),
      voice_track({0.0f, 0.0f, 1.0f}, 2300000, 2620000, 1)};
  bleed_a.signal_profile.noise_floor_db = -42.0;

  const auto result = svp::audio::reconcile_microphone_transcripts(
      {direct_a, direct_b, bleed_a});

  assert(result.speakers.size() == 3);
  assert(result.collapsed_microphone_stream_count == 0);
  assert(result.speakers[0].source_audio_stream_id == "astream_0002");
  assert(result.speakers[0].source_audio_stream_ids.size() == 1);
  assert(result.speakers[0].source_audio_stream_ids[0] == "astream_0002");
  assert(result.speakers[1].source_audio_stream_id == "astream_0004");
  assert(result.speakers[2].source_audio_stream_id == "astream_0003");
  assert(result.words.size() == 11);

  bool direct_voices_preserved = false;
  bool bleed_matched = false;
  for (const auto& evidence : result.voice_match_evidence) {
    if (evidence.left_source_ordinal == 1 &&
        evidence.right_source_ordinal == 2) {
      direct_voices_preserved = !evidence.same_voice_proven &&
                                evidence.aligned_diarized_speech_us > 0 &&
                                evidence.aligned_matching_voice_us == 0;
    }
    if (evidence.left_source_ordinal == 1 &&
        evidence.right_source_ordinal == 3) {
      bleed_matched = evidence.same_voice_proven &&
                      evidence.aligned_voice_match_ratio > 0.75 &&
                      evidence.aligned_voice_match_ratio < 0.85;
    }
  }
  assert(direct_voices_preserved);
  assert(bleed_matched);
}

void test_microphone_signal_profile_uses_diarization_negative_noise_floor() {
  const std::filesystem::path wav_path =
      std::filesystem::temp_directory_path() /
      "svp-microphone-signal-profile.wav";
  std::vector<std::int16_t> samples(32000, 100);
  std::fill(samples.begin() + 16000, samples.end(), 1000);
  write_pcm_s16le_mono_wav(wav_path, samples);

  const auto profile = svp::audio::measure_microphone_signal_profile(
      wav_path, {{1000000, 2000000}});

  assert(profile.noise_floor_db.has_value());
  assert(*profile.noise_floor_db < -49.0);
  assert(*profile.noise_floor_db > -51.0);
  assert(profile.frames.size() == 100);
  std::filesystem::remove(wav_path);
}

void test_weaker_bleed_chain_attaches_upward_without_becoming_a_speaker() {
  svp::audio::MicrophoneTranscript direct;
  direct.source_audio_stream_id = "astream_0001";
  direct.source_ordinal = 0;
  direct.words = {word("direct", 1000000, 2000000)};
  direct.word_signal_db = {-20.0};
  direct.signal_profile.noise_floor_db = -50.0;
  direct.voice_fingerprint = {1.0f, 0.0f};
  direct.voice_tracks = {voice_track({1.0f, 0.0f}, 1000000, 2000000)};

  svp::audio::MicrophoneTranscript bridge;
  bridge.source_audio_stream_id = "astream_0002";
  bridge.source_ordinal = 1;
  bridge.words = {word("bridge", 1000000, 2000000)};
  bridge.word_signal_db = {-30.0};
  bridge.signal_profile.noise_floor_db = -50.0;
  bridge.voice_fingerprint = {0.8f, 0.6f};
  bridge.voice_tracks = {voice_track({0.8f, 0.6f}, 1000000, 2000000)};

  svp::audio::MicrophoneTranscript weakest;
  weakest.source_audio_stream_id = "astream_0003";
  weakest.source_ordinal = 2;
  weakest.words = {word("weakest", 1000000, 2000000)};
  weakest.word_signal_db = {-40.0};
  weakest.signal_profile.noise_floor_db = -50.0;
  weakest.voice_fingerprint = {0.0f, 1.0f};
  weakest.voice_tracks = {voice_track({0.0f, 1.0f}, 1000000, 2000000)};

  const auto result = svp::audio::reconcile_microphone_transcripts(
      {direct, bridge, weakest});

  assert(result.speakers.size() == 3);
  assert(result.speakers[0].source_audio_stream_id == "astream_0001");
  assert(result.speakers[0].source_audio_stream_ids.size() == 1);
  assert(result.collapsed_microphone_stream_count == 0);
  assert(result.source_assignment_evidence[0].decision ==
         "independent_microphone_source");
  assert(result.source_assignment_evidence[1].decision ==
         "independent_microphone_source");
  assert(result.source_assignment_evidence[2].decision ==
         "independent_microphone_source");
}

void test_cross_anchor_duplicate_content_uses_time_local_snr_ownership() {
  svp::audio::MicrophoneTranscript microphone_a;
  microphone_a.source_audio_stream_id = "astream_0001";
  microphone_a.source_ordinal = 0;
  microphone_a.words = {
      word("alpha.", 0, 1000000),
      word("beta", 1000000, 2000000),
  };
  microphone_a.word_signal_db = {-20.0, -40.0};
  microphone_a.signal_profile.noise_floor_db = -50.0;
  microphone_a.signal_profile.frames = {
      {{0, 1000000}, -20.0},
      {{1000000, 2000000}, -40.0},
  };
  microphone_a.voice_fingerprint = {1.0f, 0.0f};
  microphone_a.voice_tracks = {voice_track({1.0f, 0.0f}, 0, 2000000)};

  svp::audio::MicrophoneTranscript microphone_b;
  microphone_b.source_audio_stream_id = "astream_0002";
  microphone_b.source_ordinal = 1;
  microphone_b.words = {
      word("alpha.", 0, 1000000),
      word("beta", 1000000, 2000000),
  };
  microphone_b.word_signal_db = {-40.0, -20.0};
  microphone_b.signal_profile.noise_floor_db = -50.0;
  microphone_b.signal_profile.frames = {
      {{0, 1000000}, -40.0},
      {{1000000, 2000000}, -20.0},
  };
  microphone_b.voice_fingerprint = {0.0f, 1.0f};
  microphone_b.voice_tracks = {voice_track({0.0f, 1.0f}, 0, 2000000)};

  const auto result = svp::audio::reconcile_microphone_transcripts(
      {microphone_a, microphone_b});

  assert(result.speakers.size() == 2);
  assert(result.words.size() == 4);
  assert(result.discarded_cross_anchor_bleed_word_count == 0);
  assert(!result.chunk_content_evidence.empty());
  assert(std::none_of(result.chunk_content_evidence.begin(),
                      result.chunk_content_evidence.end(),
                      [](const auto& evidence) {
                        return evidence.duplicate_capture_proven;
                      }));
  assert(result.words[0].text == "alpha.");
  assert(result.word_speaker_assignments[0] == "speaker_0001");
  assert(result.words[1].text == "alpha.");
  assert(result.word_speaker_assignments[1] == "speaker_0002");
}

void test_cross_anchor_ownership_does_not_switch_inside_an_utterance() {
  svp::audio::MicrophoneTranscript microphone_a;
  microphone_a.source_audio_stream_id = "astream_0001";
  microphone_a.source_ordinal = 0;
  microphone_a.words = {
      word("one", 0, 1000000),
      word("continuous", 1000000, 2000000),
      word("utterance.", 2000000, 3000000),
  };
  microphone_a.word_signal_db = {-20.0, -40.0, -20.0};
  microphone_a.signal_profile.noise_floor_db = -50.0;
  microphone_a.signal_profile.frames = {
      {{0, 1000000}, -20.0},
      {{1000000, 2000000}, -40.0},
      {{2000000, 3000000}, -20.0},
  };
  microphone_a.voice_fingerprint = {1.0f, 0.0f};
  microphone_a.voice_tracks = {
      voice_track({1.0f, 0.0f}, 0, 1000000),
      voice_track({1.0f, 0.0f}, 1000000, 3000000, 1),
  };

  svp::audio::MicrophoneTranscript microphone_b = microphone_a;
  microphone_b.source_audio_stream_id = "astream_0002";
  microphone_b.source_ordinal = 1;
  microphone_b.word_signal_db = {-40.0, -20.0, -40.0};
  microphone_b.signal_profile.frames = {
      {{0, 1000000}, -40.0},
      {{1000000, 2000000}, -20.0},
      {{2000000, 3000000}, -40.0},
  };
  microphone_b.voice_fingerprint = {0.0f, 1.0f};
  microphone_b.voice_tracks = {
      voice_track({0.0f, 1.0f}, 0, 1000000),
      voice_track({0.0f, 1.0f}, 1000000, 3000000, 1),
  };

  const auto result = svp::audio::reconcile_microphone_transcripts(
      {microphone_a, microphone_b});

  assert(result.speakers.size() == 2);
  assert(result.words.size() == 6);
  assert(result.discarded_cross_anchor_bleed_word_count == 0);
}

void test_cross_anchor_deduplication_preserves_unmatched_turn_words() {
  svp::audio::MicrophoneTranscript direct;
  direct.source_audio_stream_id = "astream_0001";
  direct.source_ordinal = 0;
  direct.words = {
      word("shared", 0, 500000),
      word("phrase", 500000, 1000000),
      word("anchor.", 1000000, 1500000),
  };
  direct.word_signal_db = {-10.0, -10.0, -10.0};
  direct.signal_profile.noise_floor_db = -50.0;
  direct.signal_profile.frames = {{{0, 1500000}, -10.0}};
  direct.voice_tracks = {voice_track({1.0f, 0.0f}, 0, 1000000)};

  svp::audio::MicrophoneTranscript other;
  other.source_audio_stream_id = "astream_0002";
  other.source_ordinal = 1;
  other.words = {
      word("shared", 0, 500000),
      word("phrase", 500000, 1000000),
      word("legitimate.", 1000000, 1500000),
  };
  other.word_signal_db = {-30.0, -30.0, -30.0};
  other.signal_profile.noise_floor_db = -50.0;
  other.signal_profile.frames = {{{0, 1500000}, -30.0}};
  other.voice_tracks = {voice_track({1.0f, 0.0f}, 0, 1000000)};

  const auto result =
      svp::audio::reconcile_microphone_transcripts({direct, other});

  assert(result.discarded_cross_anchor_bleed_word_count == 2);
  assert(result.collapsed_microphone_stream_count == 0);
  assert(result.speakers.size() == 2);
  assert(std::any_of(result.words.begin(), result.words.end(),
                     [](const svp::audio::AsrWord& candidate) {
                       return candidate.text == "legitimate.";
                     }));
}

void test_microphone_silent_inputs_are_omitted_and_ties_use_stream_order() {
  svp::audio::MicrophoneTranscript silent;
  silent.source_audio_stream_id = "astream_0001";
  silent.source_ordinal = 0;

  svp::audio::MicrophoneTranscript microphone_1;
  microphone_1.source_audio_stream_id = "astream_0002";
  microphone_1.source_ordinal = 1;
  microphone_1.words = {word("first", 1000000, 1300000)};
  microphone_1.word_signal_db = {-10.0};

  svp::audio::MicrophoneTranscript microphone_2;
  microphone_2.source_audio_stream_id = "astream_0003";
  microphone_2.source_ordinal = 2;
  microphone_2.words = {word("second", 2000000, 2300000)};
  microphone_2.word_signal_db = {-10.0};

  const auto result = svp::audio::reconcile_microphone_transcripts(
      {silent, microphone_1, microphone_2});

  assert(result.speakers.size() == 2);
  assert(result.speakers[0].source_audio_stream_id == "astream_0002");
  assert(result.speakers[0].speaker_id == "speaker_0001");
  assert(result.speakers[1].source_audio_stream_id == "astream_0003");
  assert(result.speakers[1].speaker_id == "speaker_0002");
  assert(result.word_speaker_assignments[0] == "speaker_0001");
  assert(result.word_speaker_assignments[1] == "speaker_0002");
}
