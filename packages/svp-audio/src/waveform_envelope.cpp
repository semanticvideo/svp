#include "svp/audio/waveform_envelope.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace svp::audio {
namespace {

struct PcmWavData {
  std::int32_t sample_rate = 0;
  std::vector<std::int16_t> samples;
};

std::uint16_t read_u16_le(std::istream& input) {
  std::array<unsigned char, 2> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input) {
    throw std::runtime_error("unexpected end of WAV header");
  }
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(bytes[1] << 8);
}

std::uint32_t read_u32_le(std::istream& input) {
  std::array<unsigned char, 4> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input) {
    throw std::runtime_error("unexpected end of WAV header");
  }
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string read_tag(std::istream& input) {
  std::array<char, 4> tag{};
  input.read(tag.data(), tag.size());
  if (!input) {
    throw std::runtime_error("unexpected end of WAV chunk tag");
  }
  return std::string(tag.data(), tag.size());
}

void skip_chunk(std::istream& input, std::uint32_t size) {
  input.seekg(size + (size % 2), std::ios::cur);
  if (!input) {
    throw std::runtime_error("unable to skip WAV chunk");
  }
}

PcmWavData read_pcm_s16le_mono_wav(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open analysis WAV: " + path.string());
  }

  if (read_tag(input) != "RIFF") {
    throw std::runtime_error("analysis WAV is not RIFF");
  }
  (void)read_u32_le(input);
  if (read_tag(input) != "WAVE") {
    throw std::runtime_error("analysis WAV is not WAVE");
  }

  bool fmt_seen = false;
  bool data_seen = false;
  std::uint16_t audio_format = 0;
  std::uint16_t channel_count = 0;
  std::uint32_t sample_rate = 0;
  std::uint16_t bits_per_sample = 0;
  std::vector<std::int16_t> samples;

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
      if (chunk_size < 16) {
        throw std::runtime_error("analysis WAV fmt chunk is too small");
      }
      if (chunk_size > 16) {
        skip_chunk(input, chunk_size - 16);
      }
      fmt_seen = true;
    } else if (chunk_id == "data") {
      if (chunk_size % 2 != 0) {
        throw std::runtime_error("analysis WAV data chunk is not 16-bit aligned");
      }
      const std::size_t sample_count = chunk_size / 2;
      samples.reserve(sample_count);
      for (std::size_t index = 0; index < sample_count; ++index) {
        const std::uint16_t raw = read_u16_le(input);
        samples.push_back(static_cast<std::int16_t>(raw));
      }
      if (chunk_size % 2 != 0) {
        input.seekg(1, std::ios::cur);
      }
      data_seen = true;
    } else {
      skip_chunk(input, chunk_size);
    }
  }

  if (!fmt_seen || !data_seen) {
    throw std::runtime_error("analysis WAV is missing fmt or data chunk");
  }
  if (audio_format != 1) {
    throw std::runtime_error("analysis WAV must be PCM");
  }
  if (channel_count != 1) {
    throw std::runtime_error("analysis WAV must be mono");
  }
  if (sample_rate != 16000) {
    throw std::runtime_error("analysis WAV must be 16000 Hz");
  }
  if (bits_per_sample != 16) {
    throw std::runtime_error("analysis WAV must be 16-bit PCM");
  }

  return PcmWavData{static_cast<std::int32_t>(sample_rate), std::move(samples)};
}

double db_from_normalized_amplitude(double amplitude) {
  if (amplitude <= 0.0) {
    return -120.0;
  }
  return std::max(-120.0, 20.0 * std::log10(amplitude));
}

double round_db(double value) {
  return std::round(value * 10.0) / 10.0;
}

std::string waveform_id(std::int64_t index) {
  std::ostringstream output;
  output << "wave_" << std::setw(8) << std::setfill('0') << index;
  return output.str();
}

}  // namespace

std::vector<WaveformEnvelopeRecord> generate_waveform_envelope_records(
    const std::filesystem::path& analysis_wav_path,
    std::int64_t window_duration_us) {
  if (window_duration_us <= 0) {
    throw std::invalid_argument("waveform window duration must be positive");
  }

  const PcmWavData wav = read_pcm_s16le_mono_wav(analysis_wav_path);
  const std::int64_t samples_per_window =
      (static_cast<std::int64_t>(wav.sample_rate) * window_duration_us) / 1000000;
  if (samples_per_window <= 0) {
    throw std::invalid_argument("waveform window duration is shorter than one sample");
  }

  std::vector<WaveformEnvelopeRecord> records;
  const std::int64_t sample_count = static_cast<std::int64_t>(wav.samples.size());
  for (std::int64_t window_start = 0, record_index = 0;
       window_start < sample_count;
       window_start += samples_per_window, ++record_index) {
    const std::int64_t window_end = std::min(window_start + samples_per_window,
                                            sample_count);
    long double sum_squares = 0.0;
    std::int32_t peak = 0;
    for (std::int64_t sample_index = window_start; sample_index < window_end;
         ++sample_index) {
      const std::int32_t sample = static_cast<std::int32_t>(wav.samples[sample_index]);
      const std::int32_t magnitude = std::abs(sample);
      peak = std::max(peak, magnitude);
      const long double normalized =
          static_cast<long double>(sample) / static_cast<long double>(32768.0);
      sum_squares += normalized * normalized;
    }

    const long double window_sample_count =
        static_cast<long double>(window_end - window_start);
    const double rms =
        static_cast<double>(std::sqrt(sum_squares / window_sample_count));
    const double peak_normalized =
        static_cast<double>(peak) / static_cast<double>(32768.0);

    records.push_back(WaveformEnvelopeRecord{
        record_index,
        record_index * window_duration_us,
        std::min((record_index + 1) * window_duration_us,
                 (sample_count * 1000000) / wav.sample_rate),
        round_db(db_from_normalized_amplitude(rms)),
        round_db(db_from_normalized_amplitude(peak_normalized)),
    });
  }

  return records;
}

nlohmann::json waveform_envelope_record_to_json(
    const WaveformEnvelopeRecord& record) {
  return {
      {"id", waveform_id(record.index)},
      {"start_us", record.start_us},
      {"end_us", record.end_us},
      {"rms_db", record.rms_db},
      {"peak_db", record.peak_db},
  };
}

}  // namespace svp::audio
