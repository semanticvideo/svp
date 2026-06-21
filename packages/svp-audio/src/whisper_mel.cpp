#include "svp/audio/whisper_mel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace svp::audio {
namespace {

constexpr int kSampleRate = 16000;
constexpr int kNFft = 400;
constexpr int kNHop = 160;
constexpr int kNMels = 80;
constexpr int kNFrames = 3000;
[[maybe_unused]] constexpr int kTargetSamples = kSampleRate * 30;
constexpr int kRequiredSamples = (kNFrames - 1) * kNHop + kNFft;
constexpr int kFftBins = kNFft / 2 + 1;
constexpr int kTailPadding = 50;

struct PcmWavData {
  std::vector<float> samples;
};

std::uint16_t read_u16_le(std::istream& input) {
  std::array<unsigned char, 2> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input) throw std::runtime_error("unexpected end of WAV header");
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(bytes[1] << 8);
}

std::uint32_t read_u32_le(std::istream& input) {
  std::array<unsigned char, 4> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input) throw std::runtime_error("unexpected end of WAV header");
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string read_tag(std::istream& input) {
  std::array<char, 4> tag{};
  input.read(tag.data(), tag.size());
  if (!input) throw std::runtime_error("unexpected end of WAV chunk tag");
  return std::string(tag.data(), tag.size());
}

void skip_chunk(std::istream& input, std::uint32_t size) {
  input.seekg(size + (size % 2), std::ios::cur);
}

PcmWavData read_pcm_s16le_mono_wav(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("unable to open WAV: " + path.string());

  if (read_tag(input) != "RIFF") throw std::runtime_error("WAV is not RIFF");
  (void)read_u32_le(input);
  if (read_tag(input) != "WAVE") throw std::runtime_error("WAV is not WAVE");

  bool fmt_seen = false, data_seen = false;
  std::uint16_t audio_format = 0, channel_count = 0, bits_per_sample = 0;
  std::uint32_t sample_rate = 0;
  std::vector<float> samples;

  while (input && !(fmt_seen && data_seen)) {
    const std::string chunk_id = read_tag(input);
    const std::uint32_t chunk_size = read_u32_le(input);

    if (chunk_id == "fmt ") {
      audio_format = read_u16_le(input);
      channel_count = read_u16_le(input);
      sample_rate = read_u32_le(input);
      (void)read_u32_le(input);
      (void)read_u16_le(input);
      bits_per_sample = read_u16_le(input);
      if (chunk_size > 16) skip_chunk(input, chunk_size - 16);
      fmt_seen = true;
    } else if (chunk_id == "data") {
      const std::size_t sample_count = chunk_size / 2;
      samples.reserve(sample_count);
      for (std::size_t i = 0; i < sample_count; ++i) {
        const std::uint16_t raw = read_u16_le(input);
        samples.push_back(static_cast<float>(static_cast<std::int16_t>(raw)) / 32768.0f);
      }
      data_seen = true;
    } else {
      skip_chunk(input, chunk_size);
    }
  }

  if (!fmt_seen || !data_seen) throw std::runtime_error("WAV missing fmt or data");
  if (audio_format != 1) throw std::runtime_error("WAV must be PCM");
  if (channel_count != 1) throw std::runtime_error("WAV must be mono");
  if (sample_rate != kSampleRate) throw std::runtime_error("WAV must be 16kHz");
  if (bits_per_sample != 16) throw std::runtime_error("WAV must be 16-bit");

  return {std::move(samples)};
}

void apply_hann_window(float* frame, int n) {
  for (int i = 0; i < n; ++i) {
    frame[i] *= 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / n);
  }
}

void compute_power_spectrum(const float* frame, int n_fft, float* power, int n_bins) {
  for (int k = 0; k < n_bins; ++k) {
    float real = 0.0f, imag = 0.0f;
    const float freq = 2.0f * static_cast<float>(M_PI) * k / n_fft;
    for (int n = 0; n < n_fft; ++n) {
      real += frame[n] * std::cos(freq * n);
      imag -= frame[n] * std::sin(freq * n);
    }
    power[k] = real * real + imag * imag;
  }
}

float hz_to_mel(float hz) {
  return 2595.0f * std::log10(1.0f + hz / 700.0f);
}

float mel_to_hz(float mel) {
  return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

std::vector<std::vector<float>> create_mel_filterbank() {
  const float fmin = 0.0f;
  const float fmax = static_cast<float>(kSampleRate) / 2.0f;
  const float mel_min = hz_to_mel(fmin);
  const float mel_max = hz_to_mel(fmax);

  std::vector<float> mel_points(kNMels + 2);
  for (int i = 0; i < kNMels + 2; ++i) {
    mel_points[i] = mel_min + (mel_max - mel_min) * i / (kNMels + 1);
  }

  std::vector<float> hz_points(kNMels + 2);
  for (int i = 0; i < kNMels + 2; ++i) {
    hz_points[i] = mel_to_hz(mel_points[i]);
  }

  std::vector<int> bin_points(kNMels + 2);
  for (int i = 0; i < kNMels + 2; ++i) {
    bin_points[i] = static_cast<int>(std::floor(hz_points[i] * kNFft / kSampleRate));
  }

  std::vector<std::vector<float>> filterbank(kNMels, std::vector<float>(kFftBins, 0.0f));
  for (int m = 0; m < kNMels; ++m) {
    const int left = bin_points[m];
    const int center = bin_points[m + 1];
    const int right = bin_points[m + 2];

    for (int k = left; k < center && k < kFftBins; ++k) {
      if (center > left) {
        filterbank[m][k] = static_cast<float>(k - left) / (center - left);
      }
    }
    for (int k = center; k < right && k < kFftBins; ++k) {
      if (right > center) {
        filterbank[m][k] = static_cast<float>(right - k) / (right - center);
      }
    }
  }

  return filterbank;
}

}  // namespace

WhisperMelFeatures compute_whisper_mel_from_wav(const std::filesystem::path& wav_path) {
  PcmWavData wav = read_pcm_s16le_mono_wav(wav_path);

  std::vector<float> audio = std::move(wav.samples);
  if (static_cast<int>(audio.size()) < kRequiredSamples) {
    audio.resize(kRequiredSamples, 0.0f);
  } else if (static_cast<int>(audio.size()) > kRequiredSamples) {
    audio.resize(kRequiredSamples);
  }

  const auto filterbank = create_mel_filterbank();

  std::vector<float> mel_data(kNMels * kNFrames, 0.0f);
  std::vector<float> frame(kNFft, 0.0f);
  std::vector<float> power(kFftBins, 0.0f);

  for (int t = 0; t < kNFrames; ++t) {
    const int start = t * kNHop;
    for (int i = 0; i < kNFft; ++i) {
      frame[i] = audio[start + i];
    }
    apply_hann_window(frame.data(), kNFft);
    compute_power_spectrum(frame.data(), kNFft, power.data(), kFftBins);

    for (int m = 0; m < kNMels; ++m) {
      float mel_val = 0.0f;
      for (int k = 0; k < kFftBins; ++k) {
        mel_val += filterbank[m][k] * power[k];
      }
      mel_data[m * kNFrames + t] = mel_val;
    }
  }

  float max_log = -1e10f;
  for (int m = 0; m < kNMels; ++m) {
    for (int t = 0; t < kNFrames; ++t) {
      float& val = mel_data[m * kNFrames + t];
      val = std::log10(std::max(val, 1e-10f));
      if (val > max_log) max_log = val;
    }
  }

  const float clamp_low = max_log - 8.0f;
  for (int i = 0; i < kNMels * kNFrames; ++i) {
    mel_data[i] = std::max(mel_data[i], clamp_low);
    mel_data[i] = (mel_data[i] + 4.0f) / 4.0f;
  }

  for (int t = kNFrames - kTailPadding; t < kNFrames; ++t) {
    for (int m = 0; m < kNMels; ++m) {
      mel_data[m * kNFrames + t] = 0.0f;
    }
  }

  return {std::move(mel_data), kNMels, kNFrames};
}

void write_u16_le(std::ostream& out, std::uint16_t v) {
  out.put(static_cast<char>(v & 0xFF));
  out.put(static_cast<char>((v >> 8) & 0xFF));
}

void write_u32_le(std::ostream& out, std::uint32_t v) {
  out.put(static_cast<char>(v & 0xFF));
  out.put(static_cast<char>((v >> 8) & 0xFF));
  out.put(static_cast<char>((v >> 16) & 0xFF));
  out.put(static_cast<char>((v >> 24) & 0xFF));
}

std::filesystem::path slice_wav_to_temp(
    const std::filesystem::path& input_wav,
    std::int64_t start_us,
    std::int64_t end_us,
    const std::filesystem::path& temp_dir) {
  PcmWavData wav = read_pcm_s16le_mono_wav(input_wav);

  const std::int64_t start_sample =
      static_cast<std::int64_t>(start_us * kSampleRate / 1000000LL);
  const std::int64_t end_sample =
      static_cast<std::int64_t>(end_us * kSampleRate / 1000000LL);

  const std::int64_t clamped_start = std::max(std::int64_t{0}, start_sample);
  const std::int64_t clamped_end =
      std::min(end_sample, static_cast<std::int64_t>(wav.samples.size()));

  if (clamped_start >= clamped_end) {
    throw std::runtime_error("chunk slice produces zero or negative samples");
  }

  const std::size_t n_samples =
      static_cast<std::size_t>(clamped_end - clamped_start);

  std::filesystem::create_directories(temp_dir);
  const std::filesystem::path out_path =
      temp_dir / ("chunk_slice_" +
                  std::to_string(start_us) + "_" +
                  std::to_string(end_us) + ".wav");

  std::ofstream out(out_path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("unable to create temp WAV: " + out_path.string());
  }

  const std::uint32_t data_size = static_cast<std::uint32_t>(n_samples * 2);
  const std::uint32_t riff_size = 36 + data_size;

  out.write("RIFF", 4);
  write_u32_le(out, riff_size);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  write_u32_le(out, 16);
  write_u16_le(out, 1);
  write_u16_le(out, 1);
  write_u32_le(out, static_cast<std::uint32_t>(kSampleRate));
  write_u32_le(out, static_cast<std::uint32_t>(kSampleRate * 2));
  write_u16_le(out, 2);
  write_u16_le(out, 16);
  out.write("data", 4);
  write_u32_le(out, data_size);

  for (std::size_t i = 0; i < n_samples; ++i) {
    const float s = wav.samples[static_cast<std::size_t>(clamped_start) + i];
    const std::int16_t pcm = static_cast<std::int16_t>(
        std::max(-32768.0f, std::min(32767.0f, s * 32767.0f)));
    write_u16_le(out, static_cast<std::uint16_t>(pcm));
  }

  return out_path;
}

}  // namespace svp::audio
