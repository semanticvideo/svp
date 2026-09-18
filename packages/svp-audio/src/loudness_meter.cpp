#include "svp/audio/loudness_meter.hpp"

#include "svp/media/canonical_timing.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace svp::audio {
namespace {

// BS.1770 absolute-gate threshold. ffmpeg reports its measurement floor at or
// below this value; anything at or under it means the stream had no gated
// energy and the value is unmeasurable.
constexpr double kAbsoluteGateLufs = -70.0;

// Loudness values at or below the absolute gate are not measurable signal and
// are represented as absent (JSON null) per the package schema.
std::optional<double> gated_lufs(const std::optional<double>& value) {
  if (!value.has_value() || *value <= kAbsoluteGateLufs) {
    return std::nullopt;
  }
  return value;
}

std::string loudness_id(std::int64_t index) {
  std::ostringstream output;
  output << "loud_" << std::setw(8) << std::setfill('0') << index;
  return output.str();
}

double round_db(double value) {
  return std::round(value * 10.0) / 10.0;
}

std::optional<double> linear_to_dbtp(double linear_peak) {
  if (!(linear_peak > 0.0) || !std::isfinite(linear_peak)) {
    return std::nullopt;
  }
  return round_db(20.0 * std::log10(linear_peak));
}

std::string escape_lavfi_path(const std::filesystem::path& path) {
  const std::string raw = path.string();
  std::string escaped;
  escaped.reserve(raw.size() + 8);
  for (const char ch : raw) {
    if (ch == '\'') {
      escaped += "\\'";
    } else {
      escaped += ch;
    }
  }
  return escaped;
}

std::string run_process_capture_stdout(const std::vector<std::string>& arguments) {
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
  std::string output;
  std::array<char, 65536> buffer{};
  while (true) {
    const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
    if (count > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(count));
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
    throw std::runtime_error("ffprobe loudness measurement failed with exit code " +
                             std::to_string(code));
  }

  return output;
}

std::optional<double> tag_number(const nlohmann::json& tags, const char* key) {
  const auto it = tags.find(key);
  if (it == tags.end() || !it->is_string()) {
    return std::nullopt;
  }
  try {
    const double value = std::stod(it->get<std::string>());
    if (!std::isfinite(value)) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

struct FrameMetrics {
  std::int64_t relative_end_samples = 0;
  std::int64_t end_us = 0;
  std::optional<double> momentary_lufs;
  std::optional<double> shortterm_lufs;
  std::optional<double> true_peak_linear;
  std::optional<double> integrated_lufs;
  std::optional<double> loudness_range_lu;
  std::optional<double> lra_low_lufs;
  std::optional<double> lra_high_lufs;
};

std::vector<FrameMetrics> parse_ffprobe_frames(const std::string& ffprobe_json,
                                             std::int32_t sample_rate) {
  nlohmann::json document;
  try {
    document = nlohmann::json::parse(ffprobe_json);
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error(std::string("unable to parse ffprobe loudness output: ") +
                             error.what());
  }

  const auto frames_it = document.find("frames");
  if (frames_it == document.end() || !frames_it->is_array()) {
    throw std::runtime_error("ffprobe loudness output has no frames array");
  }

  std::vector<FrameMetrics> frames;
  frames.reserve(frames_it->size());
  std::int64_t pts_origin = 0;
  bool origin_set = false;

  for (const nlohmann::json& frame : *frames_it) {
    if (!frame.is_object()) {
      continue;
    }
    const auto pts_it = frame.find("pts");
    const auto samples_it = frame.find("nb_samples");
    if (pts_it == frame.end() || !pts_it->is_number() ||
        samples_it == frame.end() || !samples_it->is_number()) {
      continue;
    }
    const std::int64_t pts = pts_it->get<std::int64_t>();
    const std::int64_t nb_samples = samples_it->get<std::int64_t>();
    if (!origin_set) {
      pts_origin = pts;
      origin_set = true;
    }

    FrameMetrics metrics;
    metrics.relative_end_samples = (pts - pts_origin) + nb_samples;
    metrics.end_us = svp::media::round_half_to_even(
        metrics.relative_end_samples * 1000000, sample_rate);

    const auto tags_it = frame.find("tags");
    if (tags_it != frame.end() && tags_it->is_object()) {
      metrics.momentary_lufs = gated_lufs(tag_number(*tags_it, "lavfi.r128.M"));
      metrics.shortterm_lufs = gated_lufs(tag_number(*tags_it, "lavfi.r128.S"));
      metrics.true_peak_linear = tag_number(*tags_it, "lavfi.r128.true_peak");
      metrics.integrated_lufs = tag_number(*tags_it, "lavfi.r128.I");
      metrics.loudness_range_lu = tag_number(*tags_it, "lavfi.r128.LRA");
      metrics.lra_low_lufs = tag_number(*tags_it, "lavfi.r128.LRA.low");
      metrics.lra_high_lufs = tag_number(*tags_it, "lavfi.r128.LRA.high");
    }
    frames.push_back(metrics);
  }

  if (frames.empty()) {
    throw std::runtime_error("ffprobe loudness output contained no usable audio frames");
  }

  return frames;
}

std::optional<double> rounded_or_null(const std::optional<double>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return round_db(*value);
}

}  // namespace

LoudnessMeasurement measure_loudness(const std::filesystem::path& ffprobe_path,
                                     const std::filesystem::path& audio_path,
                                     std::int64_t window_duration_us,
                                     std::int64_t stream_start_offset_us,
                                     std::int32_t sample_rate,
                                     std::int32_t channels,
                                     const std::string& target_id) {
  if (window_duration_us <= 0) {
    throw std::invalid_argument("loudness window duration must be positive");
  }
  if (sample_rate <= 0) {
    throw std::invalid_argument("loudness measurement requires a positive sample rate");
  }

  const std::vector<std::string> arguments{
      ffprobe_path.string(),
      "-v", "error",
      "-f", "lavfi",
      "-i",
      "amovie='" + escape_lavfi_path(audio_path) +
          "':si=0,ebur128=metadata=1:peak=true",
      "-show_frames",
      "-of", "json",
  };

  const std::string ffprobe_json = run_process_capture_stdout(arguments);
  const std::vector<FrameMetrics> frames =
      parse_ffprobe_frames(ffprobe_json, sample_rate);

  LoudnessMeasurement measurement;
  measurement.summary.target_id = target_id;
  measurement.summary.channels = channels;
  measurement.summary.sample_rate = sample_rate;

  std::int64_t stream_end_us = 0;
  double max_true_peak_linear = 0.0;
  std::optional<double> max_momentary;
  std::optional<double> max_shortterm;

  for (const FrameMetrics& frame : frames) {
    stream_end_us = std::max(stream_end_us, frame.end_us);
    if (frame.true_peak_linear.has_value()) {
      max_true_peak_linear = std::max(max_true_peak_linear, *frame.true_peak_linear);
    }
    if (frame.momentary_lufs.has_value() &&
        (!max_momentary.has_value() || *frame.momentary_lufs > *max_momentary)) {
      max_momentary = frame.momentary_lufs;
    }
    if (frame.shortterm_lufs.has_value() &&
        (!max_shortterm.has_value() || *frame.shortterm_lufs > *max_shortterm)) {
      max_shortterm = frame.shortterm_lufs;
    }
  }

  const std::int64_t window_count =
      stream_end_us > 0 ? (stream_end_us + window_duration_us - 1) / window_duration_us : 0;

  // A window's loudness comes from the latest frame inside it that carries a
  // measurement; ffmpeg can emit a partial tail frame with no r128 tags, so
  // the newest frame is not always the newest measured frame.
  std::vector<const FrameMetrics*> last_measured_frame_per_window(
      static_cast<std::size_t>(window_count), nullptr);
  std::vector<bool> window_touched(static_cast<std::size_t>(window_count), false);
  std::vector<double> max_peak_per_window(
      static_cast<std::size_t>(window_count), 0.0);

  for (const FrameMetrics& frame : frames) {
    if (frame.end_us <= 0) {
      continue;
    }
    const std::int64_t window_index = (frame.end_us - 1) / window_duration_us;
    const auto slot = static_cast<std::size_t>(window_index);
    if (slot >= last_measured_frame_per_window.size()) {
      continue;
    }
    window_touched[slot] = true;
    if (frame.momentary_lufs.has_value() || frame.shortterm_lufs.has_value()) {
      const FrameMetrics* current = last_measured_frame_per_window[slot];
      if (current == nullptr || frame.end_us >= current->end_us) {
        last_measured_frame_per_window[slot] = &frame;
      }
    }
    if (frame.true_peak_linear.has_value()) {
      max_peak_per_window[slot] =
          std::max(max_peak_per_window[slot], *frame.true_peak_linear);
    }
  }

  measurement.windows.reserve(static_cast<std::size_t>(window_count));
  for (std::int64_t window_index = 0; window_index < window_count; ++window_index) {
    const auto slot = static_cast<std::size_t>(window_index);
    if (!window_touched[slot]) {
      continue;
    }
    const FrameMetrics* last = last_measured_frame_per_window[slot];

    LoudnessWindowRecord record;
    record.index = window_index;
    record.start_us = stream_start_offset_us + window_index * window_duration_us;
    record.end_us =
        window_index == window_count - 1
            ? stream_start_offset_us + stream_end_us
            : record.start_us + window_duration_us;
    if (last != nullptr) {
      record.momentary_lufs = rounded_or_null(last->momentary_lufs);
      record.shortterm_lufs = rounded_or_null(last->shortterm_lufs);
    }
    record.true_peak_dbtp = linear_to_dbtp(max_peak_per_window[slot]);
    measurement.windows.push_back(record);
  }

  // ffmpeg may emit a final partial frame with no r128 tags; the cumulative
  // measurement lives on the last frame that carries an integrated value.
  const FrameMetrics* final_frame = nullptr;
  for (const FrameMetrics& frame : frames) {
    if (frame.integrated_lufs.has_value() &&
        (final_frame == nullptr || frame.end_us >= final_frame->end_us)) {
      final_frame = &frame;
    }
  }

  const bool has_gated_energy = final_frame != nullptr &&
                              *final_frame->integrated_lufs > kAbsoluteGateLufs;
  if (has_gated_energy) {
    measurement.summary.integrated_lufs =
        rounded_or_null(final_frame->integrated_lufs);
    measurement.summary.loudness_range_lu =
        rounded_or_null(final_frame->loudness_range_lu);
    measurement.summary.lra_low_lufs = rounded_or_null(final_frame->lra_low_lufs);
    measurement.summary.lra_high_lufs =
        rounded_or_null(final_frame->lra_high_lufs);
  }
  measurement.summary.true_peak_dbtp = linear_to_dbtp(max_true_peak_linear);
  measurement.summary.max_momentary_lufs = rounded_or_null(max_momentary);
  measurement.summary.max_shortterm_lufs = rounded_or_null(max_shortterm);

  return measurement;
}

nlohmann::json loudness_window_record_to_json(
    const LoudnessWindowRecord& record,
    std::string_view target_id,
    std::string_view processor_id) {
  return {
      {"id", loudness_id(record.index)},
      {"start_us", record.start_us},
      {"end_us", record.end_us},
      {"start_sec", svp::media::microseconds_to_seconds_string(record.start_us)},
      {"end_sec", svp::media::microseconds_to_seconds_string(record.end_us)},
      {"target_type", "audio_stream"},
      {"target_id", target_id},
      {"momentary_lufs", record.momentary_lufs.has_value()
                            ? nlohmann::json(*record.momentary_lufs)
                            : nlohmann::json(nullptr)},
      {"shortterm_lufs", record.shortterm_lufs.has_value()
                            ? nlohmann::json(*record.shortterm_lufs)
                            : nlohmann::json(nullptr)},
      {"true_peak_dbtp", record.true_peak_dbtp.has_value()
                            ? nlohmann::json(*record.true_peak_dbtp)
                            : nlohmann::json(nullptr)},
      {"processor_id", processor_id},
  };
}

nlohmann::json loudness_summary_to_json(
    const std::vector<LoudnessStreamSummary>& streams,
    std::int64_t window_us,
    std::string_view processor_id) {
  auto optional_number = [](const std::optional<double>& value) -> nlohmann::json {
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
  };

  nlohmann::json stream_entries = nlohmann::json::array();
  for (const LoudnessStreamSummary& stream : streams) {
    stream_entries.push_back({
        {"target_id", stream.target_id},
        {"integrated_lufs", optional_number(stream.integrated_lufs)},
        {"loudness_range_lu", optional_number(stream.loudness_range_lu)},
        {"lra_low_lufs", optional_number(stream.lra_low_lufs)},
        {"lra_high_lufs", optional_number(stream.lra_high_lufs)},
        {"true_peak_dbtp", optional_number(stream.true_peak_dbtp)},
        {"max_momentary_lufs", optional_number(stream.max_momentary_lufs)},
        {"max_shortterm_lufs", optional_number(stream.max_shortterm_lufs)},
        {"channels", stream.channels},
        {"sample_rate", stream.sample_rate},
    });
  }

  return {
      {"schema_version", "svp-loudness-summary-v1"},
      {"measurement_standard", "ITU-R BS.1770-4"},
      {"window_us", window_us},
      {"streams", std::move(stream_entries)},
      {"processor_id", processor_id},
  };
}

}  // namespace svp::audio
