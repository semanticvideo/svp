#pragma once

// Pin registry shared by CasPinSet (writer) and CasStore::evict (reader).

#include "file_lock.hpp"
#include "svp/exec/blake3_digest.hpp"

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace svp::exec::detail {

inline constexpr std::string_view kPinLockSuffix = ".lock";
inline constexpr std::string_view kPinListSuffix = ".pins";

struct CasPinSetState {
  std::filesystem::path root;
  std::string holder_id;
  FileLock holder_lock;
  std::vector<Blake3Digest> digests;

  ~CasPinSetState();
};

// Digests pinned by live holders. Holders whose lock is free are dead: their
// files are removed. Must be called while holding the eviction lock
// exclusively, so no holder can add a pin concurrently.
[[nodiscard]] std::error_code read_live_pins(const std::filesystem::path& root,
                                             std::set<Blake3Digest>& pinned);

}  // namespace svp::exec::detail
