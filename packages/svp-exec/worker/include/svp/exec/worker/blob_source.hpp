#pragma once

#include "svp/exec/worker/transfer_messages.hpp"

#include <filesystem>
#include <string>

namespace svp::exec::worker {

// A blob the coordinator can send: its content address and the local file
// holding exactly those bytes.
struct BlobSource {
  BlobRef ref;
  std::filesystem::path file;
};

// Hashes `file`. Throws WorkerError(io).
[[nodiscard]] BlobSource describe_blob_file(const std::filesystem::path& file);

}  // namespace svp::exec::worker
