#pragma once

#include "svp/exec/cas_store.hpp"
#include "svp/exec/worker/transfer_messages.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>

namespace svp::exec::worker {

// Receives BLOB_PUT chunks into the worker's CAS (plan §4.3: "chunked, each
// chunk and the whole blob verified"). Each chunk was already verified by the
// frame decoder against the BLAKE3 its frame header declared. Chunks of one
// blob arrive in order (offset equals the bytes received so far); when the
// last one lands the whole file is hashed and must equal the blob's BLAKE3
// before it is stored. Partial files live in this receiver's own directory
// and are deleted with it (session end), so an interrupted transfer leaves
// nothing behind.
class BlobReceiver {
 public:
  // `incoming_dir` must be private to this receiver (one per session).
  BlobReceiver(std::filesystem::path incoming_dir, CasStore cas);
  ~BlobReceiver();
  BlobReceiver(const BlobReceiver&) = delete;
  BlobReceiver& operator=(const BlobReceiver&) = delete;

  // Throws WorkerError(protocol) for an out-of-order chunk,
  // WorkerError(verification) when the completed blob does not hash to its
  // name, WorkerError(io) when it cannot be written or stored.
  void put_chunk(const BlobChunk& chunk, std::span<const std::byte> bytes);

  [[nodiscard]] bool has(const BlobRef& blob) const;
  [[nodiscard]] CasStore& cas() noexcept { return cas_; }
  [[nodiscard]] std::uint64_t blobs_completed() const noexcept { return completed_; }

 private:
  std::filesystem::path incoming_dir_;
  CasStore cas_;
  // Blob -> bytes received so far.
  std::map<BlobRef, std::uint64_t> partial_;
  std::uint64_t completed_ = 0;
};

}  // namespace svp::exec::worker
