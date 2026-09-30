#pragma once

// On-disk layout of the content-addressed store (plan §4.6):
//   <root>/blobs/b3/<2 hex>/<62 hex>
//   <root>/pending/
//   <root>/pins/
//   <root>/locks/eviction.lock

#include "svp/exec/blake3_digest.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

namespace svp::exec::detail {

// Two hex characters of fan-out give 256 shard directories, which keeps each
// directory to a few thousand entries even for a multi-million-blob cache.
inline constexpr std::size_t kCasShardHexChars = 2;

struct CasPaths {
  std::filesystem::path blobs;    // <root>/blobs/b3
  std::filesystem::path pending;  // <root>/pending
  std::filesystem::path pins;     // <root>/pins
  std::filesystem::path locks;    // <root>/locks
  std::filesystem::path eviction_lock;
};

[[nodiscard]] CasPaths cas_paths(const std::filesystem::path& root);

[[nodiscard]] std::filesystem::path cas_blob_path(const std::filesystem::path& root,
                                                  const Blake3Digest& digest);

struct CasBlobEntry {
  Blake3Digest digest{};
  std::filesystem::path path;
  std::uint64_t bytes = 0;
  std::filesystem::file_time_type last_used{};
};

// Every regular file whose shard and name spell a valid digest. Anything else
// under blobs/ (stray files, pending leftovers) is ignored.
[[nodiscard]] std::vector<CasBlobEntry> list_cas_blobs(const std::filesystem::path& root,
                                                       std::error_code& error);

}  // namespace svp::exec::detail
