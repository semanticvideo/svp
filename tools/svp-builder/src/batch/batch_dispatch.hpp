#pragma once

// Spreading a batch of videos over Macs (M6): this Mac's own slots and every
// other Mac the batch may use as a coordinator each take one video at a time;
// whichever is free takes the next one, so faster or less busy Macs build
// more of the batch. Nothing assumes how many Macs there are: with no other
// Mac the batch runs on this Mac alone, exactly as before.
//
// Another Mac's outcome (remote_video_builder.hpp) decides what happens to
// its video and to that Mac's share of the batch.
// Items are processed in their order; results land at their own index.

#include "svp/builder/remote_video_builder.hpp"

#include <chrono>
#include <filesystem>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace svp::builder::batch {

struct BatchDispatchOptions {
  std::size_t item_count = 0;
  // Videos this Mac builds at once (>= 1).
  std::size_t local_slots = 1;
  std::function<void(std::size_t index)> run_local;
  // An exception from run_local fails that item (reported here with its
  // message) instead of ending the batch.
  std::function<void(std::size_t index, const std::string& error)> on_local_error;
  std::vector<std::shared_ptr<RemoteVideoBuilder>> remote_macs;
  // Item `index` on another Mac: whatever the batch does around the build,
  // with `mac.build()` for the build itself.
  std::function<RemoteVideoOutcome(std::size_t index, RemoteVideoBuilder& mac)> run_remote;
  // How long a busy Mac waits before it asks for work again.
  std::chrono::milliseconds busy_backoff{0};
};

// Runs every item to completion and returns once all are done.
void dispatch_batch(const BatchDispatchOptions& options);

}  // namespace svp::builder::batch
