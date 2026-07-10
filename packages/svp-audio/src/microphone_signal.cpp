#include "svp/audio/microphone_transcript.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

namespace svp::audio {
namespace {

constexpr double kMinimumSignalDb = -120.0;
constexpr std::size_t kSignalReadBlockSamples = 4096;
constexpr std::uint64_t kMicrosecondsPerSecond = 1000000ULL;

struct WavLayout {
  std::uint32_t sample_rate = 0;
  std::uint64_t data_offset = 0;
  std::uint64_t data_size = 0;
};

std::uint16_t read_u16_le(std::istream& input) {
  std::array<unsigned char, 2> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input) throw std::runtime_error("truncated WAV uint16 field");
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t read_u32_le(std::istream& input) {
  std::array<unsigned char, 4> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input) throw std::runtime_error("truncated WAV uint32 field");
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

WavLayout read_wav_layout(std::ifstream& input) {
  char riff[4]{};
  char wave[4]{};
  input.read(riff, sizeof(riff));
  (void)read_u32_le(input);
  input.read(wave, sizeof(wave));
  if (!input || std::string(riff, 4) != "RIFF" ||
      std::string(wave, 4) != "WAVE") {
    throw std::runtime_error("microphone analysis input is not a RIFF/WAVE file");
  }

  bool format_found = false;
  WavLayout layout;
  while (input && !layout.data_offset) {
    char chunk_id[4]{};
    input.read(chunk_id, sizeof(chunk_id));
    if (!input) break;
    const std::uint32_t chunk_size = read_u32_le(input);
    const std::string id(chunk_id, 4);
    if (id == "fmt ") {
      const std::uint16_t format = read_u16_le(input);
      const std::uint16_t channels = read_u16_le(input);
      layout.sample_rate = read_u32_le(input);
      (void)read_u32_le(input);
      (void)read_u16_le(input);
      const std::uint16_t bits_per_sample = read_u16_le(input);
      if (format != 1 || channels != 1 || bits_per_sample != 16 ||
          layout.sample_rate == 0) {
        throw std::runtime_error(
            "microphone analysis input must be mono PCM s16le WAV");
      }
      if (chunk_size > 16) {
        input.seekg(static_cast<std::streamoff>(chunk_size - 16), std::ios::cur);
      }
      format_found = true;
    } else if (id == "data") {
      layout.data_offset = static_cast<std::uint64_t>(input.tellg());
      layout.data_size = chunk_size;
      input.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
    } else {
      input.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
    }
    if ((chunk_size & 1U) != 0U) input.seekg(1, std::ios::cur);
  }
  if (!format_found || layout.data_offset == 0) {
    throw std::runtime_error("microphone analysis WAV is missing fmt or data chunk");
  }
  return layout;
}

double measure_sample_range_db(std::ifstream& input,
                               const WavLayout& layout,
                               std::uint64_t first_sample,
                               std::uint64_t last_sample) {
  if (last_sample <= first_sample) return kMinimumSignalDb;

  input.clear();
  input.seekg(static_cast<std::streamoff>(
      layout.data_offset + first_sample * sizeof(std::int16_t)));
  std::array<std::int16_t, kSignalReadBlockSamples> buffer{};
  std::uint64_t remaining = last_sample - first_sample;
  long double sum_squares = 0.0;
  std::uint64_t sample_count = 0;
  while (remaining > 0 && input) {
    const std::size_t wanted = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(wanted * sizeof(std::int16_t)));
    const std::size_t read_count =
        static_cast<std::size_t>(input.gcount()) / sizeof(std::int16_t);
    for (std::size_t i = 0; i < read_count; ++i) {
      const long double sample = static_cast<long double>(buffer[i]);
      sum_squares += sample * sample;
    }
    sample_count += read_count;
    remaining -= read_count;
    if (read_count < wanted) break;
  }
  if (sample_count == 0 || sum_squares == 0.0) return kMinimumSignalDb;
  const double rms =
      std::sqrt(static_cast<double>(sum_squares / sample_count)) / 32768.0;
  return std::max(kMinimumSignalDb, 20.0 * std::log10(rms));
}

bool overlaps_speech(const TimeSpan& frame,
                     const std::vector<TimeSpan>& speech) {
  for (const TimeSpan& span : speech) {
    if (span.start_us >= frame.end_us) break;
    if (std::min(span.end_us, frame.end_us) >
        std::max(span.start_us, frame.start_us)) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<double> measure_word_signal_db(const std::filesystem::path& wav_path,
                                           const std::vector<AsrWord>& words) {
  std::ifstream input(wav_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open microphone analysis WAV: " +
                             wav_path.string());
  }
  const WavLayout layout = read_wav_layout(input);
  const std::uint64_t total_samples = layout.data_size / sizeof(std::int16_t);
  std::vector<double> result;
  result.reserve(words.size());

  for (const AsrWord& word : words) {
    const std::uint64_t first_sample = std::min<std::uint64_t>(
        total_samples,
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, word.start_us)) *
            layout.sample_rate / kMicrosecondsPerSecond);
    const std::uint64_t last_sample = std::min<std::uint64_t>(
        total_samples,
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, word.end_us)) *
            layout.sample_rate / kMicrosecondsPerSecond);
    if (last_sample <= first_sample) {
      result.push_back(kMinimumSignalDb);
      continue;
    }
    result.push_back(
        measure_sample_range_db(input, layout, first_sample, last_sample));
  }
  return result;
}

MicrophoneSignalProfile measure_microphone_signal_profile(
    const std::filesystem::path& wav_path,
    const std::vector<TimeSpan>& diarized_speech,
    const MicrophoneSignalPolicy& policy) {
  if (policy.frame_duration_us <= 0) {
    throw std::invalid_argument("signal frame duration must be positive");
  }
  if (policy.noise_floor_percentile < 0.0 ||
      policy.noise_floor_percentile > 1.0) {
    throw std::invalid_argument("noise floor percentile must be between 0 and 1");
  }

  std::ifstream input(wav_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open microphone analysis WAV: " +
                             wav_path.string());
  }
  const WavLayout layout = read_wav_layout(input);
  const std::uint64_t total_samples = layout.data_size / sizeof(std::int16_t);
  const std::int64_t duration_us = static_cast<std::int64_t>(
      total_samples * kMicrosecondsPerSecond / layout.sample_rate);

  std::vector<TimeSpan> sorted_speech = diarized_speech;
  std::sort(sorted_speech.begin(), sorted_speech.end(),
            [](const TimeSpan& left, const TimeSpan& right) {
              return left.start_us < right.start_us;
            });

  MicrophoneSignalProfile profile;
  std::vector<double> non_speech_levels;
  for (std::int64_t start_us = 0; start_us < duration_us;
       start_us += policy.frame_duration_us) {
    const std::int64_t end_us =
        std::min(duration_us, start_us + policy.frame_duration_us);
    const std::uint64_t first_sample = std::min<std::uint64_t>(
        total_samples, static_cast<std::uint64_t>(start_us) * layout.sample_rate /
                           kMicrosecondsPerSecond);
    const std::uint64_t last_sample = std::min<std::uint64_t>(
        total_samples, static_cast<std::uint64_t>(end_us) * layout.sample_rate /
                           kMicrosecondsPerSecond);
    MicrophoneSignalFrame frame;
    frame.timing = {start_us, end_us};
    frame.signal_db =
        measure_sample_range_db(input, layout, first_sample, last_sample);
    if (!overlaps_speech(frame.timing, sorted_speech)) {
      non_speech_levels.push_back(frame.signal_db);
    }
    profile.frames.push_back(frame);
  }

  if (!non_speech_levels.empty()) {
    std::sort(non_speech_levels.begin(), non_speech_levels.end());
    const std::size_t index = static_cast<std::size_t>(
        policy.noise_floor_percentile *
        static_cast<double>(non_speech_levels.size() - 1));
    profile.noise_floor_db = non_speech_levels[index];
  }
  return profile;
}

}  // namespace svp::audio
