#include "audio_test_support.hpp"
#include "audio_test_declarations.hpp"

#include "svp/audio/audio_extraction_executor.hpp"
#include "svp/audio/audio_stage_plan.hpp"
#include "svp/audio/spectrum_analyzer.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine_pcm(double frequency_hz, double amplitude,
                            std::int32_t sample_rate, std::int32_t channels,
                            std::int64_t frames, std::int32_t on_channel) {
  std::vector<float> pcm(static_cast<std::size_t>(frames) *
                             static_cast<std::size_t>(channels),
                         0.0F);
  for (std::int64_t i = 0; i < frames; ++i) {
    const float value = static_cast<float>(
        amplitude * std::sin(2.0 * kPi * frequency_hz *
                             static_cast<double>(i) /
                             static_cast<double>(sample_rate)));
    pcm[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) +
        static_cast<std::size_t>(on_channel)] = value;
  }
  return pcm;
}

// Index of the band containing 1000 Hz in the ten-band table.
constexpr std::size_t kBand1000 = 5;
// Index of the band containing 16000 Hz.
constexpr std::size_t kBand16000 = 9;

}  // namespace

void test_spectrum_full_scale_sine_lands_in_its_band_at_zero_dbfs() {
  const std::vector<float> pcm =
      sine_pcm(1000.0, 1.0, 48000, 1, 19200, 0);
  const svp::audio::SpectrumMeasurement measurement =
      svp::audio::measure_spectrum_pcm(pcm.data(), 19200, 1, 48000, 0,
                                     "astream_0001");

  assert(measurement.windows.size() == 1);
  const auto& bands = measurement.windows[0].bands_dbfs;
  assert(bands[kBand1000].has_value());
  assert(std::abs(*bands[kBand1000] - 0.0) < 0.5);

  // Neighboring bands see only Hann sidelobe leakage, far below the peak band.
  assert(!bands[4].has_value() || *bands[4] < -40.0);
  assert(!bands[6].has_value() || *bands[6] < -40.0);

  assert(measurement.summary.mean_band_dbfs[kBand1000].has_value());
  assert(std::abs(*measurement.summary.mean_band_dbfs[kBand1000] - 0.0) < 0.5);
  assert(measurement.summary.channels == 1);
  assert(measurement.summary.sample_rate == 48000);
}

void test_spectrum_quiet_sine_reports_band_at_negative_dbfs() {
  const std::vector<float> pcm =
      sine_pcm(1000.0, 0.5, 48000, 1, 19200, 0);
  const svp::audio::SpectrumMeasurement measurement =
      svp::audio::measure_spectrum_pcm(pcm.data(), 19200, 1, 48000, 0,
                                     "astream_0001");

  const auto& bands = measurement.windows[0].bands_dbfs;
  assert(bands[kBand1000].has_value());
  assert(std::abs(*bands[kBand1000] - (-6.0)) < 0.5);
}

void test_spectrum_window_grid_matches_loudness_timing() {
  // One second of audio -> two full 400 ms windows and a 200 ms tail.
  const std::vector<float> pcm =
      sine_pcm(1000.0, 0.25, 48000, 1, 48000, 0);
  const svp::audio::SpectrumMeasurement measurement =
      svp::audio::measure_spectrum_pcm(pcm.data(), 48000, 1, 48000, 0,
                                     "astream_0001");

  assert(measurement.windows.size() == 3);
  assert(measurement.windows[0].start_us == 0);
  assert(measurement.windows[0].end_us == 400000);
  assert(measurement.windows[1].start_us == 400000);
  assert(measurement.windows[1].end_us == 800000);
  assert(measurement.windows[2].start_us == 800000);
  assert(measurement.windows[2].end_us == 1000000);
  // The partial tail window still measures its band.
  assert(measurement.windows[2].bands_dbfs[kBand1000].has_value());
}

void test_spectrum_bands_above_nyquist_are_null() {
  // At 16 kHz the 16 kHz band sits entirely above Nyquist; the 8 kHz band
  // still resolves the low half of its range.
  const std::vector<float> pcm =
      sine_pcm(4000.0, 1.0, 16000, 1, 6400, 0);
  const svp::audio::SpectrumMeasurement measurement =
      svp::audio::measure_spectrum_pcm(pcm.data(), 6400, 1, 16000, 0,
                                     "astream_0001");

  const auto& bands = measurement.windows[0].bands_dbfs;
  assert(bands[6].has_value());  // 4 kHz band contains the sine.
  assert(!bands[kBand16000].has_value());
  assert(!measurement.summary.mean_band_dbfs[kBand16000].has_value());
}

void test_spectrum_stereo_averages_channel_energy() {
  // Sine on the left channel only: the band reads 3 dB below the mono result.
  const std::vector<float> pcm =
      sine_pcm(1000.0, 1.0, 48000, 2, 19200, 0);
  const svp::audio::SpectrumMeasurement measurement =
      svp::audio::measure_spectrum_pcm(pcm.data(), 19200, 2, 48000, 0,
                                     "astream_0001");

  const auto& bands = measurement.windows[0].bands_dbfs;
  assert(bands[kBand1000].has_value());
  assert(std::abs(*bands[kBand1000] - (-3.0)) < 0.5);
  assert(measurement.summary.channels == 2);
}

void test_spectrum_silence_reports_null_bands() {
  const std::vector<float> pcm(19200, 0.0F);
  const svp::audio::SpectrumMeasurement measurement =
      svp::audio::measure_spectrum_pcm(pcm.data(), 19200, 1, 48000, 0,
                                     "astream_0001");

  assert(measurement.windows.size() == 1);
  for (const auto& value : measurement.windows[0].bands_dbfs) {
    assert(!value.has_value());
  }
  for (const auto& value : measurement.summary.mean_band_dbfs) {
    assert(!value.has_value());
  }
}

void test_spectrum_summary_energy_averages_across_windows() {
  // Two windows: -10 dB then -20 dB in the 1 kHz band. The mean is the
  // energy average, not the arithmetic mean of the dB values.
  std::vector<float> pcm(38400, 0.0F);
  const std::vector<float> first =
      sine_pcm(1000.0, std::pow(10.0, -10.0 / 20.0), 48000, 1, 19200, 0);
  const std::vector<float> second =
      sine_pcm(1000.0, std::pow(10.0, -20.0 / 20.0), 48000, 1, 19200, 0);
  std::copy(first.begin(), first.end(), pcm.begin());
  std::copy(second.begin(), second.end(), pcm.begin() + 19200);

  const svp::audio::SpectrumMeasurement measurement =
      svp::audio::measure_spectrum_pcm(pcm.data(), 38400, 1, 48000, 0,
                                     "astream_0001");

  assert(measurement.windows.size() == 2);
  const double mean = *measurement.summary.mean_band_dbfs[kBand1000];
  const double expected =
      10.0 * std::log10((std::pow(10.0, -10.0 / 10.0) +
                         std::pow(10.0, -20.0 / 10.0)) /
                        2.0);
  assert(std::abs(mean - expected) < 0.3);
  assert(std::abs(*measurement.summary.max_band_dbfs[kBand1000] - (-10.0)) <
         0.5);
}

void test_spectrum_windows_before_origin_are_omitted() {
  // A stream that starts 100 ms before the presentation origin produces a
  // negative start_us for its first window. Stored timestamps must be
  // non-negative, so that window is omitted while the rest of the grid stays
  // intact.
  const std::vector<float> pcm =
      sine_pcm(1000.0, 1.0, 48000, 1, 57600, 0);
  const svp::audio::SpectrumMeasurement measurement =
      svp::audio::measure_spectrum_pcm(pcm.data(), 57600, 1, 48000, -100000,
                                     "astream_0001");

  assert(measurement.windows.size() == 2);
  assert(measurement.windows[0].start_us == 300000);
  assert(measurement.windows[0].end_us == 700000);
  assert(measurement.windows[1].start_us == 700000);
  assert(measurement.windows[0].bands_dbfs[kBand1000].has_value());
}

void test_spectrum_record_and_summary_json_serialization() {
  svp::audio::SpectrumWindowRecord record;
  record.index = 3;
  record.start_us = 1200000;
  record.end_us = 1600000;
  record.bands_dbfs[0] = -42.5;

  const nlohmann::json encoded = svp::audio::spectrum_window_record_to_json(
      record, "astream_0001", "proc_spectrum_octave_0001");
  assert(encoded["id"] == "spec_00000003");
  assert(encoded["start_us"] == 1200000);
  assert(encoded["end_us"] == 1600000);
  assert(encoded["target_type"] == "audio_stream");
  assert(encoded["target_id"] == "astream_0001");
  assert(encoded["bands"].size() == svp::audio::kSpectrumBandCount);
  assert(encoded["bands"][0] == -42.5);
  assert(encoded["bands"][1].is_null());
  assert(encoded["processor_id"] == "proc_spectrum_octave_0001");

  const nlohmann::json summary = svp::audio::spectrum_summary_to_json(
      {}, 400000, "proc_spectrum_octave_0001");
  assert(summary["measurement_standard"] == "IEC 61260 octave bands");
  assert(summary["window_us"] == 400000);
  assert(summary["band_centers_hz"].size() == svp::audio::kSpectrumBandCount);
  assert(summary["band_centers_hz"][0] == 31.25);
  assert(summary["streams"].empty());
  assert(summary["processor_id"] == "proc_spectrum_octave_0001");
}

void test_spectrum_plan_targets_original_streams_with_loudness_grid() {
  svp::media::MediaProbe probe;
  svp::media::VideoStreamProbe video;
  video.id = "vstream_0001";
  video.timing.timebase = {1, 1000};
  probe.video_streams.push_back(video);
  probe.audio_streams.push_back({"astream_0001", 1, "aac", 48000, 2, {}});
  probe.audio_streams[0].timing.timebase = {1, 48000};
  probe.audio_streams[0].timing.start_pts = 48000;
  probe.audio_streams[0].timing.duration_pts = 96000;

  const svp::audio::AudioStagePlan plan =
      svp::audio::build_audio_stage_plan("sample.mov", probe, true);
  const nlohmann::json encoded = svp::audio::audio_stage_plan_to_json(plan);
  const nlohmann::json spectrum = encoded["audio_extraction"]["spectrum"];

  assert(spectrum["task_id"] == "task.audio.spectrum.original_streams");
  assert(spectrum["processor_id"] == "proc_spectrum_octave_0001");
  assert(spectrum["output_ref"] == "media/audio/spectrum.jsonl");
  assert(spectrum["summary_output_ref"] == "media/audio/spectrum_summary.json");
  assert(spectrum["window_duration_us"] == 400000);
  assert(spectrum["spectrum_run"] == false);
  assert(spectrum["spectrum_written"] == false);

  const nlohmann::json targets = spectrum["targets"];
  assert(targets.size() == 1);
  assert(targets[0]["source_audio_stream_id"] == "astream_0001");
  assert(targets[0]["channels"] == 2);
  assert(targets[0]["sample_rate"] == 48000);
  assert(targets[0]["stream_start_us"] == 1000000);
  assert(targets[0]["input_ref"] == "media/audio/original_stream_000.flac");

  const std::vector<std::string> required_outputs =
      encoded["required_outputs"].get<std::vector<std::string>>();
  assert(std::find(required_outputs.begin(), required_outputs.end(),
                   "media/audio/spectrum.jsonl") != required_outputs.end());
  assert(std::find(required_outputs.begin(), required_outputs.end(),
                   "media/audio/spectrum_summary.json") !=
         required_outputs.end());
  const std::vector<std::string> pending_processors =
      encoded["pending_processors"].get<std::vector<std::string>>();
  assert(std::find(pending_processors.begin(), pending_processors.end(),
                   "spectrum_octave_bands") != pending_processors.end());
}

void test_spectrum_executor_writes_empty_artifacts_when_no_audio() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-audio-spectrum-empty-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  svp::audio::AudioExtractionPlan plan;
  plan.ffmpeg_available = true;
  plan.spectrum.task_id = "task.audio.spectrum.original_streams";
  plan.spectrum.processor_id = "proc_spectrum_octave_0001";
  plan.spectrum.output_ref = "media/audio/spectrum.jsonl";
  plan.spectrum.summary_output_ref = "media/audio/spectrum_summary.json";
  plan.processor_provenance.task_id = "task.audio.provenance.plan";
  plan.processor_provenance.output_ref = "provenance/processors.jsonl";

  const std::filesystem::path staging_root = root / "staging";
  const svp::audio::AudioExtractionRun run =
      svp::audio::execute_audio_extraction_plan(plan, staging_root);
  const nlohmann::json encoded = svp::audio::audio_extraction_run_to_json(run);

  assert(encoded["spectrum_written"] == true);
  assert(encoded["spectrum"]["written"] == true);

  const std::filesystem::path jsonl_path =
      staging_root / "media/audio/spectrum.jsonl";
  assert(std::filesystem::exists(jsonl_path));
  {
    std::ifstream input(jsonl_path);
    std::string line;
    assert(!std::getline(input, line));
  }

  const std::filesystem::path summary_path =
      staging_root / "media/audio/spectrum_summary.json";
  assert(std::filesystem::exists(summary_path));
  {
    std::ifstream input(summary_path);
    const nlohmann::json summary = nlohmann::json::parse(input);
    assert(summary["streams"].empty());
    assert(summary["band_centers_hz"].size() == svp::audio::kSpectrumBandCount);
    assert(summary["window_us"] == 400000);
  }

  {
    std::ifstream input(staging_root / "provenance/processors.jsonl");
    std::string line;
    std::getline(input, line);
    std::getline(input, line);
    std::getline(input, line);
    std::getline(input, line);
    std::getline(input, line);
    const nlohmann::json spectrum_processor = nlohmann::json::parse(line);
    assert(spectrum_processor["id"] == "proc_spectrum_octave_0001");
    assert(spectrum_processor["runtime"] == "native");
    assert(spectrum_processor["measurement_standard"] ==
           "IEC 61260 octave bands");
    assert(spectrum_processor["window_duration_us"] == 400000);
    assert(spectrum_processor["completed"] == true);
  }

  std::filesystem::remove_all(root);
}
