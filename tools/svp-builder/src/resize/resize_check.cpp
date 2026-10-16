#include "resize/resize_check.hpp"

#include "resize/cancellable_command.hpp"

#include "svp/media/media_probe.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace svp::builder::resize {
namespace {

constexpr double kMicrosecondsPerSecond = 1'000'000.0;

std::int32_t normalized_rotation(std::int32_t degrees) { return ((degrees % 360) + 360) % 360; }

std::optional<std::int64_t> seconds_us(const nlohmann::json& object) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto found = object.find("start_time");
  if (found == object.end() || !found->is_string()) {
    return std::nullopt;
  }
  try {
    return std::llround(std::stod(found->get<std::string>()) * kMicrosecondsPerSecond);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

bool audio_copies_into(const std::filesystem::path& ffmpeg_path,
                       const std::filesystem::path& source, ResizeContainer container,
                       const std::filesystem::path& scratch_dir,
                       const svp::exec::CancellationToken* cancellation) {
  const bool mp4 = container == ResizeContainer::mp4;
  const std::filesystem::path copy = scratch_dir / (mp4 ? "audio-copy.mp4" : "audio-copy.mov");
  const CommandOutcome outcome = run_cancellable_command(
      {ffmpeg_path.string(), "-nostdin", "-hide_banner", "-loglevel", "error", "-i",
       source.string(), "-map", "0:a", "-c", "copy", "-t", std::string(kAudioCopyCheckSeconds),
       "-f", mp4 ? "mp4" : "mov", "-y", copy.string()},
      scratch_dir / "audio-copy.out", scratch_dir / "audio-copy.log", cancellation);
  std::error_code ignored;
  std::filesystem::remove(copy, ignored);
  return outcome.succeeded;
}

std::optional<std::int64_t> probe_start_offset_us(const std::filesystem::path& ffprobe_path,
                                                  const std::filesystem::path& file,
                                                  const std::string& stream,
                                                  const std::filesystem::path& scratch_dir,
                                                  const svp::exec::CancellationToken* cancellation,
                                                  std::string& error) {
  const std::filesystem::path report = scratch_dir / "start-offset.json";
  const CommandOutcome outcome = run_cancellable_command(
      {ffprobe_path.string(), "-v", "error", "-select_streams", stream, "-show_entries",
       "format=start_time:stream=start_time", "-of", "json", file.string()},
      report, scratch_dir / "start-offset.log", cancellation);
  if (!outcome.succeeded) {
    error = outcome.diagnostics;
    return std::nullopt;
  }
  std::ifstream input(report, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const std::optional<std::int64_t> offset = stream_start_offset_us(text);
  if (!offset) {
    error = "ffprobe reported no start time for " + file.filename().string();
  }
  return offset;
}

std::optional<std::int64_t> stream_start_offset_us(std::string_view ffprobe_json) {
  const nlohmann::json report = nlohmann::json::parse(ffprobe_json, nullptr, false);
  if (!report.is_object() || !report.contains("streams") || !report["streams"].is_array() ||
      report["streams"].size() != 1 || !report.contains("format")) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> stream = seconds_us(report["streams"][0]);
  const std::optional<std::int64_t> format = seconds_us(report["format"]);
  if (!stream || !format) {
    return std::nullopt;
  }
  return *stream - *format;
}

std::optional<std::string> resized_media_mismatch(const svp::media::MediaIngestPlan& source_plan,
                                                  const ResizeGeometry& geometry,
                                                  const std::filesystem::path& resized_path,
                                                  const std::filesystem::path& ffprobe_path,
                                                  const std::filesystem::path& scratch_dir,
                                                  const svp::exec::CancellationToken* cancellation) {
  svp::media::MediaProbe resized;
  try {
    resized = svp::media::probe_media_with_ffprobe(resized_path, ffprobe_path);
  } catch (const std::exception& error) {
    return std::string("cannot probe the resized media: ") + error.what();
  }
  if (resized.video_streams.size() != 1) {
    return "the resized media has " + std::to_string(resized.video_streams.size()) +
           " video streams, not 1";
  }
  const svp::media::VideoStreamProbe& video = resized.video_streams.front();
  if (video.width != geometry.stored_width || video.height != geometry.stored_height) {
    return "the resized media is " + std::to_string(video.width) + "x" +
           std::to_string(video.height) + ", not " + std::to_string(geometry.stored_width) +
           "x" + std::to_string(geometry.stored_height);
  }
  const svp::media::VideoStreamProbe& source = source_plan.primary_video_stream;
  if (normalized_rotation(video.rotation_degrees) != normalized_rotation(source.rotation_degrees)) {
    return "the resized media is rotated " + std::to_string(video.rotation_degrees) +
           " degrees, the source " + std::to_string(source.rotation_degrees);
  }

  std::string error;
  const std::optional<std::int64_t> source_offset =
      probe_start_offset_us(ffprobe_path, source_plan.source_path, std::to_string(source.index),
                            scratch_dir, cancellation, error);
  if (!source_offset) {
    return "cannot read the source's start time: " + error;
  }
  const std::optional<std::int64_t> resized_offset =
      probe_start_offset_us(ffprobe_path, resized_path, "v:0", scratch_dir, cancellation, error);
  if (!resized_offset) {
    return "cannot read the resized media's start time: " + error;
  }
  if (std::llabs(*resized_offset - *source_offset) > kStartOffsetToleranceUs) {
    return "the resized video starts " + std::to_string(*resized_offset) +
           " us after its file start, the source's " + std::to_string(*source_offset) + " us";
  }
  return std::nullopt;
}

}  // namespace svp::builder::resize
