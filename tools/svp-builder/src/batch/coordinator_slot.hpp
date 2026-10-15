#pragma once

// This Mac's video coordinator slot (kVideoBuildSlotsPerMac), shared by every
// `svp-builder build --coordinators` process on the Mac: the one that holds it
// coordinates its video here, and the others hand theirs to another Mac
// (video_placement.hpp). It is an advisory lock (flock) on one file, so it is
// released when its holder ends, however it ends.

#include "svp/exec/cancellation_token.hpp"

#include <filesystem>
#include <optional>

namespace svp::builder::batch {

// $SVP_COORDINATOR_SLOT, else ~/Library/Application Support/SVP/Coordinator/slot.lock.
[[nodiscard]] std::filesystem::path default_coordinator_slot_path();

class CoordinatorSlot {
 public:
  // The slot when no other process holds it, otherwise nullopt. Throws
  // std::runtime_error when the lock file cannot be created or opened.
  [[nodiscard]] static std::optional<CoordinatorSlot> try_take(const std::filesystem::path& path);
  // Waits for the slot; nullopt when `cancellation` is requested first.
  [[nodiscard]] static std::optional<CoordinatorSlot> take(
      const std::filesystem::path& path, const svp::exec::CancellationToken* cancellation);

  CoordinatorSlot(CoordinatorSlot&& other) noexcept;
  CoordinatorSlot& operator=(CoordinatorSlot&& other) noexcept;
  CoordinatorSlot(const CoordinatorSlot&) = delete;
  CoordinatorSlot& operator=(const CoordinatorSlot&) = delete;
  ~CoordinatorSlot();

 private:
  explicit CoordinatorSlot(int fd) noexcept : fd_(fd) {}
  int fd_ = -1;
};

}  // namespace svp::builder::batch
