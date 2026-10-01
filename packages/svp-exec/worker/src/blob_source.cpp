#include "svp/exec/worker/blob_source.hpp"

#include "svp/exec/worker/worker_error.hpp"
#include "svp/models/hash.hpp"

#include <system_error>

namespace svp::exec::worker {

BlobSource describe_blob_file(const std::filesystem::path& file) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(file, error);
  if (error) {
    throw WorkerError(WorkerErrorCode::io, "cannot read " + file.string() + ": " + error.message());
  }
  std::string hex;
  try {
    hex = svp::models::blake3_hex_for_file(file);
  } catch (const std::exception& failure) {
    throw WorkerError(WorkerErrorCode::io, failure.what());
  }
  const std::optional<Blake3Digest> digest = parse_blake3_hex(hex);
  if (!digest) {
    throw WorkerError(WorkerErrorCode::io, "cannot hash " + file.string());
  }
  return BlobSource{.ref = BlobRef{.blake3 = *digest, .bytes = static_cast<std::uint64_t>(size)},
                    .file = file};
}

}  // namespace svp::exec::worker
