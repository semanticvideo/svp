#pragma once

// Whether freshly written resized media (resized_source.hpp) is what was asked
// for: one video stream of the planned size and the source's rotation, whose
// first frame starts as long after the file's start as the source's primary
// video does, so the two files share one timeline and a time in one is the
// same moment in the other.

#include "resize/resize_geometry.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/media/media_ingest_plan.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svp::builder::resize {

// How far the resized video's start offset may be from the source's: the
// encoder time base rounds it (resize_encoder.hpp, kFineTimeBase).
inline constexpr std::int64_t kStartOffsetToleranceUs = 20;

// nullopt when `resized_path` is what was asked for, otherwise why not.
// `scratch_dir` holds ffprobe's output.
[[nodiscard]] std::optional<std::string> resized_media_mismatch(
    const svp::media::MediaIngestPlan& source_plan, const ResizeGeometry& geometry,
    const std::filesystem::path& resized_path, const std::filesystem::path& ffprobe_path,
    const std::filesystem::path& scratch_dir, const svp::exec::CancellationToken* cancellation);

// The start offset in microseconds of an ffprobe JSON report of one stream
// (-show_entries format=start_time:stream=start_time -of json): the stream's
// start_time minus the format's. nullopt when either is missing.
[[nodiscard]] std::optional<std::int64_t> stream_start_offset_us(std::string_view ffprobe_json);

}  // namespace svp::builder::resize
