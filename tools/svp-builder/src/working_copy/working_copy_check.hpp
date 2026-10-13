#pragma once

// Whether a freshly written working copy can stand in for the source's
// primary video in frame analysis (working_copy.hpp). A copy that cannot is
// discarded and the source is analysed instead.

#include "working_copy/working_copy_geometry.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/media/media_ingest_plan.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svp::builder::working_copy {

// nullopt when the copy at `copy_path` can stand in for the source's primary
// video, otherwise why not. It can when its video stream has the planned
// stored size and the source's rotation, and its first frame starts as long
// after the file's own start as the source's primary video does: decoders
// seek (-ss) relative to the file's start, so equal offsets put every seek
// on the frame with the same timestamp in both files. `scratch_dir` holds
// ffprobe's output.
[[nodiscard]] std::optional<std::string> working_copy_mismatch(
    const svp::media::MediaIngestPlan& source_plan, const WorkingCopyGeometry& geometry,
    const std::filesystem::path& copy_path, const std::filesystem::path& ffprobe_path,
    const std::filesystem::path& scratch_dir, const svp::exec::CancellationToken* cancellation);

// The start offset in microseconds of an ffprobe JSON report of one stream
// (-show_entries format=start_time:stream=start_time -of json): the stream's
// start_time minus the format's. nullopt when either is missing.
[[nodiscard]] std::optional<std::int64_t> stream_start_offset_us(std::string_view ffprobe_json);

}  // namespace svp::builder::working_copy
