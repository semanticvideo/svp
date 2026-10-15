#include "batch/video_placement.hpp"

#include <set>
#include <thread>

namespace svp::builder::batch {
namespace {

bool cancelled(const svp::exec::CancellationToken* cancellation) {
  return cancellation != nullptr && cancellation->requested();
}

// Waits `duration`, waking early when the build is cancelled.
void pause(std::chrono::milliseconds duration, const svp::exec::CancellationToken* cancellation) {
  constexpr std::chrono::milliseconds kStep{100};
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until && !cancelled(cancellation)) {
    std::this_thread::sleep_for(kStep);
  }
}

}  // namespace

VideoPlacement place_video(const VideoPlacementOptions& options) {
  const auto report = [&options](const std::string& line) {
    if (options.report) {
      options.report(line);
    }
  };
  std::set<std::string> unavailable;
  bool reported_busy = false;
  while (!cancelled(options.cancellation)) {
    if (std::optional<CoordinatorSlot> slot = CoordinatorSlot::try_take(options.slot_path)) {
      return VideoPlacement{.where = VideoPlacement::Where::here, .slot = std::move(slot)};
    }
    if (!reported_busy) {
      report("this Mac is coordinating another video; asking the other coordinators");
      reported_busy = true;
    }
    bool asked = false;
    for (const std::shared_ptr<RemoteVideoBuilder>& mac : options.coordinators) {
      if (cancelled(options.cancellation)) {
        break;
      }
      const std::string name = mac->name();
      if (unavailable.contains(name)) {
        continue;
      }
      asked = true;
      RemoteVideoOutcome outcome = mac->build(options.request);
      switch (outcome.status) {
        case RemoteVideoStatus::built:
        case RemoteVideoStatus::failed:
          return VideoPlacement{.where = VideoPlacement::Where::other_mac,
                                .mac = name,
                                .outcome = std::move(outcome)};
        case RemoteVideoStatus::busy:
          break;
        case RemoteVideoStatus::unavailable:
          unavailable.insert(name);
          report(name + " cannot coordinate it: " + outcome.message);
          break;
      }
    }
    if (!asked) {
      // No Mac can take it: it waits for this Mac's slot.
      report("no other coordinator can take it; waiting for this Mac");
      if (std::optional<CoordinatorSlot> slot =
              CoordinatorSlot::take(options.slot_path, options.cancellation)) {
        return VideoPlacement{.where = VideoPlacement::Where::here, .slot = std::move(slot)};
      }
      break;
    }
    pause(options.busy_backoff, options.cancellation);
  }
  return VideoPlacement{.where = VideoPlacement::Where::cancelled};
}

}  // namespace svp::builder::batch
