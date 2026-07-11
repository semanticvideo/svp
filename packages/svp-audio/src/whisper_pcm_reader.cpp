#include "svp/audio/whisper_pcm_reader.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace svp::audio {
namespace {

std::uint16_t read_u16(std::istream& input) {
  std::array<unsigned char, 2> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t read_u32(std::istream& input) {
  std::array<unsigned char, 4> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::string read_tag(std::istream& input) {
  std::array<char, 4> tag{};
  input.read(tag.data(), tag.size());
  return std::string(tag.data(), tag.size());
}

}  // namespace

std::vector<float> read_whisper_pcm16_mono_wav(
    const std::filesystem::path& wav_path) {
  std::ifstream input(wav_path, std::ios::binary);
  if (!input || read_tag(input) != "RIFF") {
    throw std::runtime_error("Whisper input is not a RIFF WAV: " +
                             wav_path.string());
  }
  (void)read_u32(input);
  if (read_tag(input) != "WAVE") {
    throw std::runtime_error("Whisper input is not a WAVE file: " +
                             wav_path.string());
  }

  bool format_found = false;
  std::uint32_t data_size = 0;
  std::streampos data_offset{};
  while (input && (!format_found || data_size == 0)) {
    const std::string chunk = read_tag(input);
    if (!input) break;
    const std::uint32_t size = read_u32(input);
    if (chunk == "fmt ") {
      const std::uint16_t format = read_u16(input);
      const std::uint16_t channels = read_u16(input);
      const std::uint32_t sample_rate = read_u32(input);
      (void)read_u32(input);
      (void)read_u16(input);
      const std::uint16_t bits_per_sample = read_u16(input);
      if (format != 1 || channels != 1 || sample_rate != 16000 ||
          bits_per_sample != 16) {
        throw std::runtime_error(
            "Whisper input must be 16 kHz mono PCM16 WAV");
      }
      format_found = true;
      if (size > 16) input.seekg(size - 16, std::ios::cur);
    } else if (chunk == "data") {
      data_offset = input.tellg();
      data_size = size;
      input.seekg(size, std::ios::cur);
    } else {
      input.seekg(size, std::ios::cur);
    }
    if ((size & 1U) != 0U) input.seekg(1, std::ios::cur);
  }
  if (!format_found || data_size == 0 || (data_size % 2U) != 0U) {
    throw std::runtime_error("Whisper input WAV has no valid PCM data");
  }

  input.clear();
  input.seekg(data_offset);
  std::vector<std::int16_t> pcm(data_size / 2U);
  input.read(reinterpret_cast<char*>(pcm.data()), data_size);
  if (!input) throw std::runtime_error("Failed to read Whisper input PCM");

  std::vector<float> samples;
  samples.reserve(pcm.size());
  for (const std::int16_t sample : pcm) {
    samples.push_back(static_cast<float>(sample) / 32768.0F);
  }
  return samples;
}

}  // namespace svp::audio
