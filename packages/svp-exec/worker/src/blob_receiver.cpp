#include "svp/exec/worker/blob_receiver.hpp"

#include "svp/exec/worker/worker_error.hpp"
#include "svp/models/hash.hpp"

#include <fstream>
#include <system_error>

namespace svp::exec::worker {
namespace {

std::filesystem::path partial_path(const std::filesystem::path& directory, const BlobRef& blob) {
  return directory / (blake3_hex(blob.blake3) + ".partial");
}

}  // namespace

BlobReceiver::BlobReceiver(std::filesystem::path incoming_dir, CasStore cas)
    : incoming_dir_(std::move(incoming_dir)), cas_(std::move(cas)) {
  std::error_code error;
  std::filesystem::create_directories(incoming_dir_, error);
  if (error) {
    throw WorkerError(WorkerErrorCode::io,
                      "cannot create " + incoming_dir_.string() + ": " + error.message());
  }
}

BlobReceiver::~BlobReceiver() {
  std::error_code error;
  std::filesystem::remove_all(incoming_dir_, error);
}

bool BlobReceiver::has(const BlobRef& blob) const { return cas_.has(blob.blake3); }

void BlobReceiver::put_chunk(const BlobChunk& chunk, std::span<const std::byte> bytes) {
  const BlobRef& blob = chunk.blob;
  const std::uint64_t received = partial_.contains(blob) ? partial_.at(blob) : 0;
  if (chunk.offset != received) {
    throw WorkerError(WorkerErrorCode::protocol,
                      "BLOB_PUT for " + blake3_hex(blob.blake3) + " at offset " +
                          std::to_string(chunk.offset) + " but " + std::to_string(received) +
                          " bytes were received so far");
  }
  const std::filesystem::path partial = partial_path(incoming_dir_, blob);
  {
    std::ofstream out(partial, std::ios::binary | (received == 0 ? std::ios::trunc : std::ios::app));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
      throw WorkerError(WorkerErrorCode::io, "cannot write " + partial.string());
    }
  }
  const std::uint64_t now = received + bytes.size();
  if (now < blob.bytes) {
    partial_[blob] = now;
    return;
  }
  partial_.erase(blob);
  std::error_code error;
  std::string hex;
  try {
    hex = svp::models::blake3_hex_for_file(partial);
  } catch (const std::exception& failure) {
    std::filesystem::remove(partial, error);
    throw WorkerError(WorkerErrorCode::io, failure.what());
  }
  if (hex != blake3_hex(blob.blake3)) {
    std::filesystem::remove(partial, error);
    throw WorkerError(WorkerErrorCode::verification,
                      "received blob " + blake3_hex(blob.blake3) + " hashes to " + hex);
  }
  CacheResult<Blake3Digest> stored = cas_.put_file(partial);
  std::filesystem::remove(partial, error);
  if (!stored || stored.value() != blob.blake3) {
    throw WorkerError(WorkerErrorCode::io,
                      "cannot store blob " + blake3_hex(blob.blake3) + " in the worker cache" +
                          (stored ? std::string() : ": " + stored.error().message));
  }
  ++completed_;
}

}  // namespace svp::exec::worker
