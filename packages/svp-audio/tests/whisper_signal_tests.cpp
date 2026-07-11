#include "audio_test_support.hpp"
#include "svp/audio/whisper_model_metadata.hpp"
#include "svp/audio/whisper_untimestamped_words.hpp"

void test_whisper_runtime_available_reports_honestly() {
  const bool available = svp::audio::is_whisper_runtime_available();
#ifdef SVP_AUDIO_ONNX_RUNTIME_AVAILABLE
  assert(available);
#else
  assert(!available);
#endif
}

void test_whisper_inference_blocks_when_model_dir_missing() {
  const std::filesystem::path fake_dir =
      std::filesystem::temp_directory_path() / "svp-whisper-fake-model";
  std::filesystem::remove_all(fake_dir);
  std::filesystem::create_directories(fake_dir);

  const std::filesystem::path fake_wav = fake_dir / "test.wav";
  {
    std::ofstream output(fake_wav, std::ios::binary);
    write_u32_le(output, 0x46464952);
    write_u32_le(output, 36 + 16000 * 2);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    write_u32_le(output, 16);
    write_u16_le(output, 1);
    write_u16_le(output, 1);
    write_u32_le(output, 16000);
    write_u32_le(output, 32000);
    write_u16_le(output, 2);
    write_u16_le(output, 16);
    output.write("data", 4);
    write_u32_le(output, 16000 * 2);
    for (int i = 0; i < 16000; ++i) {
      write_u16_le(output, 0);
    }
  }

  const svp::audio::WhisperInferenceResult result =
      svp::audio::run_whisper_inference(fake_wav, fake_dir, "test_chunk", 0, 1000000);

  assert(!result.ran);
  assert(!result.blockers.empty());

  std::filesystem::remove_all(fake_dir);
}

void test_whisper_mel_30s_chunk_produces_valid_output_without_overread() {
  // A 30-second chunk at 16 kHz is exactly 480000 samples.
  // The STFT loop needs (kNFrames-1)*kNHop + kNFft = 480240 samples.
  // The old code resized to 480000, causing a 240-sample buffer overread
  // in the last STFT frame (t=2999, start=479840, reads through 480239).
  std::vector<std::int16_t> samples(480000, 16384);
  const auto wav_path =
      std::filesystem::temp_directory_path() / "svp_mel_test_30s.wav";
  write_pcm_s16le_mono_wav(wav_path, samples);

  svp::audio::WhisperMelFeatures features =
      svp::audio::compute_whisper_mel_from_wav(wav_path);
  assert(features.n_mels == 80);
  assert(features.n_frames == 3000);
  assert(static_cast<int>(features.data.size()) == 80 * 3000);

  for (float v : features.data) {
    assert(std::isfinite(v));
  }

  // A full-length input consumes the model's complete frame capacity, so no
  // real audio may be replaced by decoder tail padding.
  float max_last_audio_frame = -std::numeric_limits<float>::max();
  for (int m = 0; m < 80; ++m) {
    max_last_audio_frame =
        std::max(max_last_audio_frame, features.data[m * 3000 + 2999]);
  }
  assert(max_last_audio_frame != 0.0f);

  std::filesystem::remove(wav_path);
}

void test_whisper_mel_buffer_includes_samples_for_last_stft_frame() {
  // The old code resized the audio buffer to 480000 samples. The STFT loop
  // needs 480240 samples for all 3000 frames. Without the fix, frame 2999
  // reads 240 samples past the buffer end (undefined behavior).
  // This test verifies the function completes safely with exactly 480000
  // samples (the old buffer size) and produces valid output.
  std::vector<std::int16_t> samples(480000, 16384);
  const auto wav_path =
      std::filesystem::temp_directory_path() / "svp_mel_test_tail.wav";
  write_pcm_s16le_mono_wav(wav_path, samples);

  svp::audio::WhisperMelFeatures features =
      svp::audio::compute_whisper_mel_from_wav(wav_path);
  assert(features.n_mels == 80);
  assert(features.n_frames == 3000);

  // All values must be finite (no NaN/inf from buffer overread garbage).
  for (float v : features.data) {
    assert(std::isfinite(v));
  }

  // The final frame must carry signal; tail padding must never overwrite a
  // full-length input.
  float max_last_audio_frame = -std::numeric_limits<float>::max();
  for (int m = 0; m < 80; ++m) {
    max_last_audio_frame =
        std::max(max_last_audio_frame, features.data[m * 3000 + 2999]);
  }
  assert(max_last_audio_frame != 0.0f);

  std::filesystem::remove(wav_path);
}

void test_whisper_mel_uses_log10_for_compression() {
  // Whisper and sherpa-onnx both use log10 (not natural log) for mel
  // power compression. For a constant signal of amplitude ~0.5, the DC
  // bin power is roughly (0.5 * 200)^2 = 10000. After log10: log10(10000) = 4.0.
  // The final normalization is (val + 4) / 4, giving:
  //   log10: (4.0 + 4) / 4 = 2.0
  //   ln:    (9.21 + 4) / 4 ≈ 3.30
  // A threshold of 2.5 confirms log10 is used (max ≈ 2.0, not 3.3).
  std::vector<std::int16_t> samples(16000, 16384);
  const auto wav_path =
      std::filesystem::temp_directory_path() / "svp_mel_test_log.wav";
  write_pcm_s16le_mono_wav(wav_path, samples);

  svp::audio::WhisperMelFeatures features =
      svp::audio::compute_whisper_mel_from_wav(wav_path);
  assert(features.n_mels == 80);
  assert(features.n_frames > 1000);
  assert(features.n_frames < 3000);

  float max_val = -std::numeric_limits<float>::max();
  for (float v : features.data) {
    max_val = std::max(max_val, v);
  }

  // With log10, max_val should be ≈2.0. With natural log, ≈3.3.
  // Threshold of 2.5 confirms log10 is used.
  assert(max_val < 2.5f);

  for (int m = 0; m < 80; ++m) {
    assert(features.data[m * features.n_frames +
                         (features.n_frames - 1)] == 0.0f);
  }

  std::filesystem::remove(wav_path);
}

void test_softmax_probability_for_token_basic() {
  // Two logits: token 0 has logit 0, token 1 has logit 0.
  // Softmax should give 0.5 for each.
  std::vector<float> logits = {0.0f, 0.0f};
  double p0 = svp::audio::softmax_probability_for_token(logits, 0);
  double p1 = svp::audio::softmax_probability_for_token(logits, 1);
  assert(std::abs(p0 - 0.5) < 1e-9);
  assert(std::abs(p1 - 0.5) < 1e-9);

  // Token 0 has much higher logit -> probability close to 1.
  logits = {10.0f, 0.0f};
  p0 = svp::audio::softmax_probability_for_token(logits, 0);
  p1 = svp::audio::softmax_probability_for_token(logits, 1);
  assert(p0 > 0.9999);
  assert(p1 < 0.0001);
  assert(p0 + p1 > 0.9999 && p0 + p1 < 1.0001);
}

void test_softmax_probability_for_token_edge_cases() {
  // Empty logits -> 0.0
  std::vector<float> empty;
  assert(svp::audio::softmax_probability_for_token(empty, 0) == 0.0);

  // Out-of-range token_id -> 0.0
  std::vector<float> logits = {1.0f, 2.0f, 3.0f};
  assert(svp::audio::softmax_probability_for_token(logits, -1) == 0.0);
  assert(svp::audio::softmax_probability_for_token(logits, 3) == 0.0);

  // Single token -> probability 1.0
  logits = {5.0f};
  double p = svp::audio::softmax_probability_for_token(logits, 0);
  assert(std::abs(p - 1.0) < 1e-9);
}

void test_aggregate_word_confidence_mean() {
  std::vector<double> token_probs = {0.8, 0.6, 0.9, 0.3};
  // Mean of tokens 0,1,2 = (0.8+0.6+0.9)/3 = 0.7666...
  std::vector<std::size_t> indices = {0, 1, 2};
  double conf = svp::audio::aggregate_word_confidence(token_probs, indices);
  assert(std::abs(conf - (0.8 + 0.6 + 0.9) / 3.0) < 1e-9);

  // Single token
  indices = {3};
  conf = svp::audio::aggregate_word_confidence(token_probs, indices);
  assert(std::abs(conf - 0.3) < 1e-9);
}

void test_aggregate_word_confidence_empty() {
  std::vector<double> token_probs;
  std::vector<std::size_t> indices;
  assert(svp::audio::aggregate_word_confidence(token_probs, indices) == 0.0);

  token_probs = {0.5, 0.7};
  indices = {};
  assert(svp::audio::aggregate_word_confidence(token_probs, indices) == 0.0);

  indices = {5};  // out of range
  assert(svp::audio::aggregate_word_confidence(token_probs, indices) == 0.0);
}

void test_whisper_control_tokens_are_loaded_from_model_metadata() {
  const auto tokens = svp::audio::parse_whisper_control_tokens({
      {"sot_sequence", "50257"},
      {"eot", "50256"},
      {"no_speech", "50361"},
      {"no_timestamps", "50362"},
      {"translate", "50357"},
      {"blank_id", "220"},
  });
  assert(tokens.sot_sequence == std::vector<int>({50257}));
  assert(tokens.no_timestamps == 50362);
  assert(tokens.timestamp_begin() == 50363);
}

void test_untimestamped_whisper_words_preserve_token_confidence() {
  const auto root = std::filesystem::temp_directory_path() /
                    "svp-untimestamped-whisper-word-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto tokens_path = root / "tokens.txt";
  {
    std::ofstream output(tokens_path);
    output << "IEhlbGxv 0\n";
    output << "IHdvcmxk 1\n";
  }
  svp::audio::WhisperTokenTable token_table;
  assert(token_table.load(tokens_path));

  const auto words = svp::audio::decode_untimestamped_whisper_words(
      {0, 1}, {0.8, 0.6}, token_table, 0, 1000000, 100);
  assert(words.size() == 2);
  assert(words[0].text == "Hello");
  assert(std::abs(words[0].confidence - 0.8) < 1e-9);
  assert(words[1].text == "world");
  assert(std::abs(words[1].confidence - 0.6) < 1e-9);
  std::filesystem::remove_all(root);
}
