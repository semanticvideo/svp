#pragma once

// Where one video of `svp-builder build --coordinators` is coordinated
// (M6 design: each Mac coordinates one video at a time, and its build already
// spreads over every Mac's measured slots).
//
// While this Mac's coordinator slot (coordinator_slot.hpp) is free, the video
// is coordinated here, exactly as without the option. While another build on
// this Mac holds the slot, the video goes to the first listed Mac that takes
// it (paired_video_builder.hpp): that Mac coordinates it, distributed over its
// own paired workers when the build is distributed, and the package and run
// report come back to this Mac. A busy Mac (coordinating another video) is
// asked again after a pause; one that cannot take whole-video jobs is not
// asked again for this video. With every listed Mac unavailable, the video
// waits for this Mac's slot. A video never fails only because every Mac was
// busy: it waits for whichever frees first.

#include "batch/coordinator_slot.hpp"

#include "svp/builder/remote_video_builder.hpp"
#include "svp/exec/cancellation_token.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder::batch {

struct VideoPlacementOptions {
  std::filesystem::path slot_path;
  std::vector<std::shared_ptr<RemoteVideoBuilder>> coordinators;
  RemoteVideoRequest request;
  // How long to wait before asking busy Macs again.
  std::chrono::milliseconds busy_backoff{0};
  const svp::exec::CancellationToken* cancellation = nullptr;
  // One line per decision (which Mac, busy, unavailable), for stderr.
  std::function<void(const std::string&)> report;
};

struct VideoPlacement {
  enum class Where { here, other_mac, cancelled };
  Where where = Where::cancelled;
  // Here: this Mac's slot, held until the local build ends.
  std::optional<CoordinatorSlot> slot;
  // Another Mac: which one, and how its build ended (built or failed).
  std::string mac;
  RemoteVideoOutcome outcome;
};

// Decides where the video is coordinated and, when that is another Mac, runs
// it there. Throws std::runtime_error when the slot file cannot be opened.
[[nodiscard]] VideoPlacement place_video(const VideoPlacementOptions& options);

}  // namespace svp::builder::batch
