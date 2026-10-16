#pragma once

// Files a worker's tasks made for their coordinator to fetch (BLOB_GET): kept
// in the worker's content-addressed cache and pinned there, so cache
// eviction cannot remove one before it is fetched; the pins go with the
// worker session.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cas_pin.hpp"
#include "svp/exec/cas_store.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace svp::builder::workers {

class CasOutputPins {
 public:
  // `holder` names the pin set: one per session and task type.
  CasOutputPins(std::filesystem::path cas_root, std::string holder);

  // Stores `file` in the worker cache and pins it. Throws std::runtime_error.
  svp::exec::Blake3Digest store(const std::filesystem::path& file);

 private:
  std::filesystem::path cas_root_;
  std::string holder_;
  std::mutex mutex_;
  std::optional<svp::exec::CasStore> store_;
  std::optional<svp::exec::CasPinSet> pins_;
};

}  // namespace svp::builder::workers
