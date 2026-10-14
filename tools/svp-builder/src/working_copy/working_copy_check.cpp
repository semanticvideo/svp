#include "working_copy/working_copy_check.hpp"

#include "working_copy/cancellable_command.hpp"

#include "svp/media/media_probe.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>

namespace svp::builder::working_copy {
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

// The start offset of stream `stream` (an ffprobe stream specifier) of `file`.
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

}  // namespace

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

std::optional<std::string> working_copy_mismatch(const svp::media::MediaIngestPlan& source_plan,
                                                 const WorkingCopyGeometry& geometry,
                                                 const std::filesystem::path& copy_path,
                                                 const std::filesystem::path& ffprobe_path,
                                                 const std::filesystem::path& scratch_dir,
                                                 const svp::exec::CancellationToken* cancellation) {
  svp::media::MediaProbe copy;
  try {
    copy = svp::media::probe_media_with_ffprobe(copy_path, ffprobe_path);
  } catch (const std::exception& error) {
    return std::string("cannot probe the working copy: ") + error.what();
  }
  if (copy.video_streams.size() != 1) {
    return "the working copy has " + std::to_string(copy.video_streams.size()) +
           " video streams, not 1";
  }
  const svp::media::VideoStreamProbe& video = copy.video_streams.front();
  if (video.width != geometry.stored_width || video.height != geometry.stored_height) {
    return "the working copy is " + std::to_string(video.width) + "x" +
           std::to_string(video.height) + ", not " + std::to_string(geometry.stored_width) +
           "x" + std::to_string(geometry.stored_height);
  }
  const svp::media::VideoStreamProbe& source = source_plan.primary_video_stream;
  if (normalized_rotation(video.rotation_degrees) != normalized_rotation(source.rotation_degrees)) {
    return "the working copy is rotated " + std::to_string(video.rotation_degrees) +
           " degrees, the source " + std::to_string(source.rotation_degrees);
  }

  std::string error;
  const std::optional<std::int64_t> source_offset =
      probe_start_offset_us(ffprobe_path, source_plan.source_path, std::to_string(source.index),
                            scratch_dir, cancellation, error);
  if (!source_offset) {
    return "cannot read the source's start time: " + error;
  }
  const std::optional<std::int64_t> copy_offset =
      probe_start_offset_us(ffprobe_path, copy_path, "v:0", scratch_dir, cancellation, error);
  if (!copy_offset) {
    return "cannot read the working copy's start time: " + error;
  }
  if (*copy_offset != *source_offset) {
    return "the working copy's video starts " + std::to_string(*copy_offset) +
           " us after its file start, the source's " + std::to_string(*source_offset) + " us";
  }
  return std::nullopt;
}

}  // namespace svp::builder::working_copy
