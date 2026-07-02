#include "private.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace svp::audio::sherpa_diarization_internal {

std::vector<float> read_pcm_s16le_mono_wav_samples(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("unable to open WAV: " + path.string());

  char riff[4];
  input.read(riff, 4);
  if (std::memcmp(riff, "RIFF", 4) != 0) throw std::runtime_error("WAV is not RIFF");

  std::uint32_t file_size = 0;
  input.read(reinterpret_cast<char*>(&file_size), 4);

  char wave[4];
  input.read(wave, 4);
  if (std::memcmp(wave, "WAVE", 4) != 0) throw std::runtime_error("WAV is not WAVE");

  bool fmt_seen = false, data_seen = false;
  std::uint16_t audio_format = 0, channel_count = 0, bits_per_sample = 0;
  std::uint32_t sample_rate = 0;
  std::vector<float> samples;

  while (input && !(fmt_seen && data_seen)) {
    char chunk_id[4];
    input.read(chunk_id, 4);
    if (!input) break;

    std::uint32_t chunk_size = 0;
    input.read(reinterpret_cast<char*>(&chunk_size), 4);
    if (!input) break;

    if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
      input.read(reinterpret_cast<char*>(&audio_format), 2);
      input.read(reinterpret_cast<char*>(&channel_count), 2);
      input.read(reinterpret_cast<char*>(&sample_rate), 4);
      input.seekg(6, std::ios::cur);
      input.read(reinterpret_cast<char*>(&bits_per_sample), 2);
      if (chunk_size > 16) input.seekg(chunk_size - 16, std::ios::cur);
      fmt_seen = true;
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      const std::size_t sample_count = chunk_size / 2;
      samples.reserve(sample_count);
      for (std::size_t i = 0; i < sample_count; ++i) {
        std::int16_t raw = 0;
        input.read(reinterpret_cast<char*>(&raw), 2);
        samples.push_back(static_cast<float>(raw) / 32768.0f);
      }
      data_seen = true;
    } else {
      input.seekg(chunk_size + (chunk_size % 2), std::ios::cur);
    }
  }

  if (!fmt_seen || !data_seen) throw std::runtime_error("WAV missing fmt or data");
  if (audio_format != 1) throw std::runtime_error("WAV must be PCM");
  if (channel_count != 1) throw std::runtime_error("WAV must be mono");
  if (sample_rate != 16000) throw std::runtime_error("WAV must be 16kHz");
  if (bits_per_sample != 16) throw std::runtime_error("WAV must be 16-bit");

  return samples;
}

PcmS16MonoWavInfo read_pcm_s16le_mono_wav_info(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("unable to open WAV: " + path.string());

  char riff[4];
  input.read(riff, 4);
  if (std::memcmp(riff, "RIFF", 4) != 0) throw std::runtime_error("WAV is not RIFF");

  std::uint32_t file_size = 0;
  input.read(reinterpret_cast<char*>(&file_size), 4);

  char wave[4];
  input.read(wave, 4);
  if (std::memcmp(wave, "WAVE", 4) != 0) throw std::runtime_error("WAV is not WAVE");

  bool fmt_seen = false, data_seen = false;
  std::uint16_t audio_format = 0, channel_count = 0, bits_per_sample = 0;
  std::uint32_t sample_rate = 0;
  PcmS16MonoWavInfo info;
  info.path = path;

  while (input && !(fmt_seen && data_seen)) {
    char chunk_id[4];
    input.read(chunk_id, 4);
    if (!input) break;

    std::uint32_t chunk_size = 0;
    input.read(reinterpret_cast<char*>(&chunk_size), 4);
    if (!input) break;

    if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
      input.read(reinterpret_cast<char*>(&audio_format), 2);
      input.read(reinterpret_cast<char*>(&channel_count), 2);
      input.read(reinterpret_cast<char*>(&sample_rate), 4);
      input.seekg(6, std::ios::cur);
      input.read(reinterpret_cast<char*>(&bits_per_sample), 2);
      if (chunk_size > 16) input.seekg(chunk_size - 16, std::ios::cur);
      fmt_seen = true;
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      info.data_offset = input.tellg();
      info.sample_count = static_cast<std::size_t>(chunk_size / 2);
      input.seekg(chunk_size + (chunk_size % 2), std::ios::cur);
      data_seen = true;
    } else {
      input.seekg(chunk_size + (chunk_size % 2), std::ios::cur);
    }
  }

  if (!fmt_seen || !data_seen) throw std::runtime_error("WAV missing fmt or data");
  if (audio_format != 1) throw std::runtime_error("WAV must be PCM");
  if (channel_count != 1) throw std::runtime_error("WAV must be mono");
  if (sample_rate != 16000) throw std::runtime_error("WAV must be 16kHz");
  if (bits_per_sample != 16) throw std::runtime_error("WAV must be 16-bit");

  return info;
}

std::vector<float> read_pcm_s16le_mono_wav_range(
    const PcmS16MonoWavInfo& info,
    std::size_t start_sample,
    std::size_t end_sample) {
  if (start_sample >= end_sample || start_sample >= info.sample_count) return {};
  end_sample = std::min(end_sample, info.sample_count);

  std::ifstream input(info.path, std::ios::binary);
  if (!input) throw std::runtime_error("unable to open WAV: " + info.path.string());
  input.seekg(info.data_offset + static_cast<std::streamoff>(start_sample * 2),
              std::ios::beg);

  std::vector<float> samples;
  samples.reserve(end_sample - start_sample);
  for (std::size_t i = start_sample; i < end_sample; ++i) {
    std::int16_t raw = 0;
    input.read(reinterpret_cast<char*>(&raw), 2);
    if (!input) break;
    samples.push_back(static_cast<float>(raw) / 32768.0f);
  }
  return samples;
}


}  // namespace svp::audio::sherpa_diarization_internal
