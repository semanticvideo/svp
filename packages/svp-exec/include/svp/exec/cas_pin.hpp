#pragma once

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cache_error.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace svp::exec {

namespace detail {
struct CasPinSetState;
}

// Blobs a live holder (a build journal or worker session) still needs. RC2
// §20.5.2: eviction MUST NOT remove artifacts referenced by a live journal.
//
// On disk, under <cache root>/pins/:
//   <holder_id>.lock   held with an exclusive advisory lock while this object
//                      lives; eviction treats the holder as live only while
//                      that lock is held, so a crashed holder's pins expire
//                      with its process.
//   <holder_id>.pins   one lowercase hex digest per line, replaced atomically
//                      on every add().
//
// Created by CasStore::pin_set(). Destruction removes both files.
class CasPinSet {
 public:
  CasPinSet(CasPinSet&&) noexcept;
  CasPinSet& operator=(CasPinSet&&) noexcept;
  CasPinSet(const CasPinSet&) = delete;
  CasPinSet& operator=(const CasPinSet&) = delete;
  ~CasPinSet();

  [[nodiscard]] const std::string& holder_id() const noexcept;
  [[nodiscard]] const std::vector<Blake3Digest>& digests() const noexcept;

  // Records the pin, serialized against eviction, then reports not_found when
  // the blob is not (or no longer) stored. The pin is kept either way, so a
  // later put() of the same digest is protected.
  CacheStatus add(const Blake3Digest& digest);

 private:
  friend class CasStore;
  explicit CasPinSet(std::unique_ptr<detail::CasPinSetState> state);

  std::unique_ptr<detail::CasPinSetState> state_;
};

}  // namespace svp::exec
