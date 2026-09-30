#pragma once

#include "svp/exec/journal_records.hpp"

#include <filesystem>
#include <string>

namespace svp::exec {

// Streams `path` through BLAKE3 and records its size and modification time.
// Throws JournalError(io_error) when the file cannot be read.
[[nodiscard]] SourceFingerprintRecord fingerprint_source(std::string source_id,
                                                         const std::filesystem::path& path);

}  // namespace svp::exec
