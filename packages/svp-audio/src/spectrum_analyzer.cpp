#include "svp/audio/spectrum_analyzer.hpp"

#include "svp/media/canonical_timing.hpp"

#include <nlohmann/json.hpp>

#include "pocketfft/pocketfft_hdronly.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace svp::audio {
namespace {

constexpr double kSqrt2 = 1.4142135623730950488;
constexpr double kPi = 3.14159265358979323846;

// A Hann-windowed full-scale sine reads exactly 0 dBFS after this correction:
// Parseval folds its mean-square (1/2) into the normalization factor.
//   power = 2 * folded_bin_energy / (N * window_power)
// where folded_bin_energy counts DC/Nyquist bins once and interior bins twice.
constexpr double kDbfsPowerFactor = 2.0;

std::string spectrum_id(std::int64_t index) {
  std::ostringstream output;
  output << "spec_" << std::setw(8) << std::setfill('0') << index;
  return output.str();
}

double round_db(double value) {
  return std::round(value * 10.0) / 10.0;
}

// Returns the [low, high) bin range whose center frequencies fall inside
// [low_hz, high_hz) for an N-point transform at the given rate. High is
// inclusive of the Nyquist bin when the edge reaches it.
struct BinRange {
  std::int64_t first = 0;
  std::int64_t last_exclusive = 0;
};

BinRange band_bin_range(double low_hz, double high_hz, std::int64_t fft_size,
                        std::int32_t sample_rate) {
  const double bin_hz = static_cast<double>(sample_rate) /
                        static_cast<double>(fft_size);
  const std::int64_t bin_count = fft_size / 2 + 1;
  BinRange range;
  range.first = static_cast<std::int64_t>(
      std::ceil(low_hz / bin_hz - 1e-9));
  range.last_exclusive = static_cast<std::int64_t>(
      std::ceil(high_hz / bin_hz - 1e-9));
  range.first = std::clamp<std::int64_t>(range.first, 0, bin_count);
  range.last_exclusive =
      std::clamp<std::int64_t>(range.last_exclusive, 0, bin_count);
  return range;
}

// Incremental analyzer: consumes interleaved frames, emits one record per
// 400 ms grid window, and accumulates per-band summary energy. Window sample
// boundaries are rounded from the microsecond grid per index, so windows never
// drift relative to the loudness layer.
class SpectrumEngine {
 public:
  SpectrumEngine(std::int32_t channels, std::int32_t sample_rate,
                 std::int64_t stream_start_offset_us)
      : channels_(channels),
        sample_rate_(sample_rate),
        stream_start_offset_us_(stream_start_offset_us) {
    window_samples_.resize(static_cast<std::size_t>(channels));
  }

  void accept(const float* interleaved_pcm, std::int64_t frame_count) {
    std::int64_t consumed = 0;
    while (consumed < frame_count) {
      const std::int64_t window_end_frame =
          svp::media::round_half_to_even(
              (window_index_ + 1) * kSpectrumWindowDurationUs * sample_rate_,
              1000000);
      const std::int64_t take =
          std::min(frame_count - consumed, window_end_frame - next_frame_);
      for (std::int64_t frame = 0; frame < take; ++frame) {
        for (std::int32_t ch = 0; ch < channels_; ++ch) {
          window_samples_[static_cast<std::size_t>(ch)].push_back(
              interleaved_pcm[(consumed + frame) * channels_ + ch]);
        }
      }
      consumed += take;
      next_frame_ += take;
      if (next_frame_ == window_end_frame) {
        emit_window(false);
      }
    }
  }

  SpectrumMeasurement finish(const std::string& target_id) {
    if (!window_samples_.empty() && !window_samples_[0].empty()) {
      emit_window(true);
    }
    SpectrumMeasurement measurement;
    measurement.windows = std::move(records_);
    measurement.summary.target_id = target_id;
    measurement.summary.channels = channels_;
    measurement.summary.sample_rate = sample_rate_;
    for (std::size_t band = 0; band < kSpectrumBandCount; ++band) {
      if (band_measured_windows_[band] > 0) {
        measurement.summary.mean_band_dbfs[band] = round_db(
            10.0 * std::log10(band_power_sum_[band] /
                              static_cast<double>(band_measured_windows_[band])));
      }
      if (band_power_max_[band] > 0.0) {
        measurement.summary.max_band_dbfs[band] =
            round_db(10.0 * std::log10(band_power_max_[band]));
      }
    }
    return measurement;
  }

 private:
  void emit_window(bool final_partial) {
    const std::int64_t sample_count =
        static_cast<std::int64_t>(window_samples_[0].size());
    SpectrumWindowRecord record;
    record.index = window_index_;
    record.start_us =
        stream_start_offset_us_ + window_index_ * kSpectrumWindowDurationUs;
    const std::int64_t window_end_us = svp::media::round_half_to_even(
        next_frame_ * 1000000, sample_rate_);
    record.end_us = final_partial ? stream_start_offset_us_ + window_end_us
                                  : record.start_us + kSpectrumWindowDurationUs;

    if (sample_count >= 2) {
      measure_bands(sample_count, record);
    }

    records_.push_back(record);
    window_index_ += 1;
    for (auto& channel : window_samples_) {
      channel.clear();
    }
  }

  void measure_bands(std::int64_t sample_count, SpectrumWindowRecord& record) {
    const std::int64_t n = sample_count;
    const std::int64_t bin_count = n / 2 + 1;
    const double nyquist = static_cast<double>(sample_rate_) / 2.0;

    // Periodic Hann, matching standard STFT convention.
    std::vector<double> window(static_cast<std::size_t>(n));
    double window_power = 0.0;
    for (std::int64_t i = 0; i < n; ++i) {
      const double w =
          0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                               static_cast<double>(n));
      window[static_cast<std::size_t>(i)] = w;
      window_power += w * w;
    }

    std::array<double, kSpectrumBandCount> channel_power_sum{};
    std::array<BinRange, kSpectrumBandCount> ranges{};
    for (std::size_t band = 0; band < kSpectrumBandCount; ++band) {
      const double center = kSpectrumBandCentersHz[band];
      const double low = center / kSqrt2;
      const double high = std::min(center * kSqrt2, nyquist);
      if (low >= nyquist) {
        continue;
      }
      ranges[band] = band_bin_range(low, high, n, sample_rate_);
    }

    std::array<double, kSpectrumBandCount> power_sum{};
    std::array<std::int64_t, kSpectrumBandCount> power_contrib{};

    std::vector<float> fft_in(static_cast<std::size_t>(n));
    std::vector<std::complex<float>> fft_out(static_cast<std::size_t>(bin_count));

    for (std::int32_t ch = 0; ch < channels_; ++ch) {
      const auto& samples = window_samples_[static_cast<std::size_t>(ch)];
      for (std::int64_t i = 0; i < n; ++i) {
        fft_in[static_cast<std::size_t>(i)] =
            samples[static_cast<std::size_t>(i)] *
            static_cast<float>(window[static_cast<std::size_t>(i)]);
      }
      pocketfft::r2c({static_cast<std::size_t>(n)},
                     {static_cast<std::ptrdiff_t>(sizeof(float))},
                     {static_cast<std::ptrdiff_t>(sizeof(std::complex<float>))},
                     0, true, fft_in.data(), fft_out.data(), 1.0F);

      for (std::size_t band = 0; band < kSpectrumBandCount; ++band) {
        const BinRange range = ranges[band];
        if (range.last_exclusive <= range.first) {
          continue;
        }
        double folded = 0.0;
        for (std::int64_t k = range.first; k < range.last_exclusive; ++k) {
          const double magnitude2 =
              static_cast<double>(fft_out[static_cast<std::size_t>(k)].real()) *
                  static_cast<double>(
                      fft_out[static_cast<std::size_t>(k)].real()) +
              static_cast<double>(fft_out[static_cast<std::size_t>(k)].imag()) *
                  static_cast<double>(
                      fft_out[static_cast<std::size_t>(k)].imag());
          const bool edge_bin = (k == 0) || (k == bin_count - 1 && n % 2 == 0);
          folded += edge_bin ? magnitude2 : 2.0 * magnitude2;
        }
        const double power =
            kDbfsPowerFactor * folded / (static_cast<double>(n) * window_power);
        power_sum[band] += power;
        power_contrib[band] += 1;
      }
    }

    for (std::size_t band = 0; band < kSpectrumBandCount; ++band) {
      if (power_contrib[band] == 0 || power_sum[band] <= 0.0) {
        record.bands_dbfs[band] = std::nullopt;
        continue;
      }
      const double mean_power =
          power_sum[band] / static_cast<double>(power_contrib[band]);
      record.bands_dbfs[band] = round_db(10.0 * std::log10(mean_power));
      band_power_sum_[band] += mean_power;
      band_measured_windows_[band] += 1;
      band_power_max_[band] = std::max(band_power_max_[band], mean_power);
    }
  }

  std::int32_t channels_;
  std::int32_t sample_rate_;
  std::int64_t stream_start_offset_us_;
  std::int64_t next_frame_ = 0;
  std::int64_t window_index_ = 0;
  std::vector<std::vector<float>> window_samples_;
  std::vector<SpectrumWindowRecord> records_;
  std::array<double, kSpectrumBandCount> band_power_sum_{};
  std::array<double, kSpectrumBandCount> band_power_max_{};
  std::array<std::int64_t, kSpectrumBandCount> band_measured_windows_{};
};

// Streams decoded PCM bytes into the engine so a long file never has to fit
// in memory. Bytes that split a float or a channel frame carry over to the
// next read.
void run_decode_feeding_engine(const std::vector<std::string>& arguments,
                               SpectrumEngine& engine, std::int32_t channels) {
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
  }

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
  }

  if (pid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    close(pipe_fds[1]);
    execvp(argv[0], argv.data());
    _exit(127);
  }

  close(pipe_fds[1]);
  const std::int64_t frame_bytes =
      static_cast<std::int64_t>(channels) * static_cast<std::int64_t>(sizeof(float));
  std::vector<char> carry;
  carry.reserve(static_cast<std::size_t>(frame_bytes));
  std::array<char, 65536> buffer{};
  while (true) {
    const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
    if (count > 0) {
      carry.insert(carry.end(), buffer.data(), buffer.data() + count);
      const std::int64_t complete =
          static_cast<std::int64_t>(carry.size()) / frame_bytes * frame_bytes;
      if (complete > 0) {
        engine.accept(reinterpret_cast<const float*>(carry.data()),
                      complete / frame_bytes);
        carry.erase(carry.begin(), carry.begin() + complete);
      }
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  close(pipe_fds[0]);

  int status = 0;
  pid_t waited = 0;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);

  if (waited < 0) {
    throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    throw std::runtime_error("ffmpeg spectrum decode failed with exit code " +
                             std::to_string(code));
  }
}

}  // namespace

SpectrumMeasurement measure_spectrum_pcm(const float* interleaved_pcm,
                                         std::int64_t frame_count,
                                         std::int32_t channels,
                                         std::int32_t sample_rate,
                                         std::int64_t stream_start_offset_us,
                                         const std::string& target_id) {
  if (channels <= 0) {
    throw std::invalid_argument("spectrum measurement requires at least one channel");
  }
  if (sample_rate <= 0) {
    throw std::invalid_argument("spectrum measurement requires a positive sample rate");
  }
  SpectrumEngine engine(channels, sample_rate, stream_start_offset_us);
  engine.accept(interleaved_pcm, frame_count);
  return engine.finish(target_id);
}

SpectrumMeasurement measure_spectrum(const std::filesystem::path& ffmpeg_path,
                                     const std::filesystem::path& audio_path,
                                     std::int64_t stream_start_offset_us,
                                     std::int32_t sample_rate,
                                     std::int32_t channels,
                                     const std::string& target_id) {
  if (channels <= 0) {
    throw std::invalid_argument("spectrum measurement requires at least one channel");
  }
  if (sample_rate <= 0) {
    throw std::invalid_argument("spectrum measurement requires a positive sample rate");
  }

  const std::vector<std::string> arguments{
      ffmpeg_path.string(),
      "-v", "error",
      "-i", audio_path.string(),
      "-map", "0:a:0",
      "-f", "f32le",
      "-acodec", "pcm_f32le",
      "-",
  };

  SpectrumEngine engine(channels, sample_rate, stream_start_offset_us);
  run_decode_feeding_engine(arguments, engine, channels);
  return engine.finish(target_id);
}

nlohmann::json spectrum_window_record_to_json(
    const SpectrumWindowRecord& record,
    std::string_view target_id,
    std::string_view processor_id) {
  nlohmann::json bands = nlohmann::json::array();
  for (const std::optional<double>& value : record.bands_dbfs) {
    bands.push_back(value.has_value() ? nlohmann::json(*value)
                                      : nlohmann::json(nullptr));
  }
  return {
      {"id", spectrum_id(record.index)},
      {"start_us", record.start_us},
      {"end_us", record.end_us},
      {"start_sec", svp::media::microseconds_to_seconds_string(record.start_us)},
      {"end_sec", svp::media::microseconds_to_seconds_string(record.end_us)},
      {"target_type", "audio_stream"},
      {"target_id", target_id},
      {"bands", std::move(bands)},
      {"processor_id", processor_id},
  };
}

nlohmann::json spectrum_summary_to_json(
    const std::vector<SpectrumStreamSummary>& streams,
    std::int64_t window_us,
    std::string_view processor_id) {
  auto optional_number = [](const std::optional<double>& value) -> nlohmann::json {
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
  };
  auto band_array = [&optional_number](
      const std::array<std::optional<double>, kSpectrumBandCount>& values) {
    nlohmann::json bands = nlohmann::json::array();
    for (const std::optional<double>& value : values) {
      bands.push_back(optional_number(value));
    }
    return bands;
  };

  nlohmann::json stream_entries = nlohmann::json::array();
  for (const SpectrumStreamSummary& stream : streams) {
    stream_entries.push_back({
        {"target_id", stream.target_id},
        {"mean_band_dbfs", band_array(stream.mean_band_dbfs)},
        {"max_band_dbfs", band_array(stream.max_band_dbfs)},
        {"channels", stream.channels},
        {"sample_rate", stream.sample_rate},
    });
  }

  return {
      {"schema_version", "svp-spectrum-summary-v1"},
      {"measurement_standard", "IEC 61260 octave bands"},
      {"window_us", window_us},
      {"band_centers_hz", kSpectrumBandCentersHz},
      {"streams", std::move(stream_entries)},
      {"processor_id", processor_id},
  };
}

}  // namespace svp::audio
