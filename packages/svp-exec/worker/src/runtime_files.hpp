#pragma once

#include <filesystem>
#include <string_view>

namespace svp::exec::worker::detail {

// Mode a runtime file gets on disk: executables live in a `bin/` directory
// (svp-builder, the bundled ffmpeg and ffprobe); everything else (libraries,
// licenses, records) is read-only data. The mode is not part of the runtime
// identity; the bytes are.
[[nodiscard]] std::filesystem::perms runtime_file_mode(std::string_view relative_path);

void write_text_file(const std::filesystem::path& path, std::string_view bytes);

}  // namespace svp::exec::worker::detail
