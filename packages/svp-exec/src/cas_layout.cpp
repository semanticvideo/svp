#include "cas_layout.hpp"

#include <string>

namespace svp::exec::detail {
namespace {

namespace fs = std::filesystem;

std::optional<Blake3Digest> digest_from_entry(const fs::path& shard, const fs::path& file) {
  const std::string shard_name = shard.filename().string();
  if (shard_name.size() != kCasShardHexChars) {
    return std::nullopt;
  }
  return parse_blake3_hex(shard_name + file.filename().string());
}

}  // namespace

CasPaths cas_paths(const fs::path& root) {
  CasPaths paths{.blobs = root / "blobs" / "b3",
                 .pending = root / "pending",
                 .pins = root / "pins",
                 .locks = root / "locks"};
  paths.eviction_lock = paths.locks / "eviction.lock";
  return paths;
}

fs::path cas_blob_path(const fs::path& root, const Blake3Digest& digest) {
  const std::string hex = blake3_hex(digest);
  return cas_paths(root).blobs / hex.substr(0, kCasShardHexChars) /
         hex.substr(kCasShardHexChars);
}

std::vector<CasBlobEntry> list_cas_blobs(const fs::path& root, std::error_code& error) {
  std::vector<CasBlobEntry> entries;
  error.clear();
  for (fs::directory_iterator shard(cas_paths(root).blobs, error), end; !error && shard != end;
       shard.increment(error)) {
    std::error_code type_error;
    if (!shard->is_directory(type_error)) {
      continue;
    }
    std::error_code shard_error;
    for (fs::directory_iterator file(shard->path(), shard_error);
         !shard_error && file != fs::directory_iterator(); file.increment(shard_error)) {
      const auto digest = digest_from_entry(shard->path(), file->path());
      std::error_code entry_error;
      if (!digest || !file->is_regular_file(entry_error)) {
        continue;
      }
      const std::uintmax_t size = file->file_size(entry_error);
      const fs::file_time_type last_used = file->last_write_time(entry_error);
      if (entry_error) {
        // Removed by a concurrent eviction between listing and stat.
        continue;
      }
      entries.push_back(CasBlobEntry{
          .digest = *digest, .path = file->path(), .bytes = size, .last_used = last_used});
    }
    if (shard_error && shard_error != std::errc::no_such_file_or_directory) {
      error = shard_error;
      return entries;
    }
  }
  return entries;
}

}  // namespace svp::exec::detail
