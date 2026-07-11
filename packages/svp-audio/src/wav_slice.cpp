#include "svp/audio/wav_slice.hpp"

#include "svp/audio/whisper_pcm_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace svp::audio {
namespace {

constexpr std::int64_t kWhisperSampleRate = 16000;

void write_u16_le(std::ostream& output, std::uint16_t value) {
  output.put(static_cast<char>(value & 0xFF));
  output.put(static_cast<char>((value >> 8) & 0xFF));
}

void write_u32_le(std::ostream& output, std::uint32_t value) {
  output.put(static_cast<char>(value & 0xFF));
  output.put(static_cast<char>((value >> 8) & 0xFF));
  output.put(static_cast<char>((value >> 16) & 0xFF));
  output.put(static_cast<char>((value >> 24) & 0xFF));
}

}  // namespace

std::filesystem::path slice_wav_to_temp(
    const std::filesystem::path& input_wav,
    std::int64_t start_us,
    std::int64_t end_us,
    const std::filesystem::path& temp_dir) {
  const std::vector<float> samples = read_whisper_pcm16_mono_wav(input_wav);
  const std::int64_t start_sample =
      start_us * kWhisperSampleRate / 1000000LL;
  const std::int64_t end_sample =
      end_us * kWhisperSampleRate / 1000000LL;
  const std::int64_t clamped_start = std::max<std::int64_t>(0, start_sample);
  const std::int64_t clamped_end = std::min<std::int64_t>(
      end_sample, static_cast<std::int64_t>(samples.size()));
  if (clamped_start >= clamped_end) {
    throw std::runtime_error("chunk slice produces zero or negative samples");
  }

  const std::size_t sample_count =
      static_cast<std::size_t>(clamped_end - clamped_start);
  std::filesystem::create_directories(temp_dir);
  const std::filesystem::path output_path =
      temp_dir / ("chunk_slice_" + std::to_string(start_us) + "_" +
                  std::to_string(end_us) + ".wav");
  std::ofstream output(output_path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("unable to create temp WAV: " +
                             output_path.string());
  }

  const std::uint32_t data_size =
      static_cast<std::uint32_t>(sample_count * sizeof(std::int16_t));
  output.write("RIFF", 4);
  write_u32_le(output, 36 + data_size);
  output.write("WAVE", 4);
  output.write("fmt ", 4);
  write_u32_le(output, 16);
  write_u16_le(output, 1);
  write_u16_le(output, 1);
  write_u32_le(output, static_cast<std::uint32_t>(kWhisperSampleRate));
  write_u32_le(output, static_cast<std::uint32_t>(kWhisperSampleRate * 2));
  write_u16_le(output, 2);
  write_u16_le(output, 16);
  output.write("data", 4);
  write_u32_le(output, data_size);

  for (std::int64_t index = clamped_start; index < clamped_end; ++index) {
    const float scaled = samples[static_cast<std::size_t>(index)] * 32767.0f;
    const auto pcm = static_cast<std::int16_t>(
        std::clamp(scaled, -32768.0f, 32767.0f));
    write_u16_le(output, static_cast<std::uint16_t>(pcm));
  }
  return output_path;
}

}  // namespace svp::audio
