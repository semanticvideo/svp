#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace svp::vision::tasks {

// Identity of the ffmpeg build an ocr.frame_batch task decodes with (plan
// §2.4 item 10: frames come from ffmpeg, so a worker must decode with the
// coordinator's ffmpeg or output can differ). It is "b3:" and the BLAKE3 of
// everything `<ffmpeg> -version` prints: the version, the toolchain, the
// configure line, and the version of every FFmpeg library it loaded. Two
// installs of the same build (a runtime bundle's static ffmpeg, or the same
// package manager bottle on two Macs) print the same text and so share an
// identity; any other build does not.
//
// nullopt when the program cannot be run or prints nothing. Runs the
// program once per call; callers cache it.
[[nodiscard]] std::optional<std::string> ffmpeg_build_identity(
    const std::filesystem::path& ffmpeg);

// ffmpeg_build_identity, computed once per distinct path for the life of the
// process (thread-safe).
[[nodiscard]] std::optional<std::string> cached_ffmpeg_build_identity(
    const std::filesystem::path& ffmpeg);

}  // namespace svp::vision::tasks
