#pragma once

// Another Mac that builds whole videos for this Mac's batch (M6): what a
// batch (build-batch, interlace create-batch) asks of it and what comes back.
// The paired-worker implementation is Apple-only (batch/paired_video_builder
// .hpp); a batch without other Macs has none.

#include "svp/builder/video_build_parameters.hpp"
#include "svp/exec/lease_policy.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace svp::builder::batch {

// How a video another Mac took went:
//   * built or failed: the video is done (a failed build is reported, the
//     batch goes on);
//   * busy: it is coordinating another video right now (slot sharing turned
//     the job away); the video goes back to the front of the queue and that
//     Mac asks again after the busy backoff;
//   * unavailable: it cannot be reached, refused the session, or cannot
//     build this runtime's videos; the video goes back to the front of the
//     queue and that Mac takes no more of this batch.
// How often a busy Mac (one that coordinates another video right now) is
// asked again: once per lease floor (lease_policy.hpp, 30 s). A whole video
// keeps a Mac busy for many minutes, so asking more often would only open
// and close connections, and a Mac that frees up stays idle at most this
// long out of a build that lasts far longer.
inline constexpr std::chrono::milliseconds kRemoteVideoBusyBackoff =
    svp::exec::kDefaultLeaseFloor;

enum class RemoteVideoStatus { built, failed, busy, unavailable };

struct RemoteVideoOutcome {
  RemoteVideoStatus status = RemoteVideoStatus::unavailable;
  std::string message;
};

// What a batch asks another Mac to build: one video, with the options that
// shape its package, delivered to `output_path` (replaced atomically once
// verified) and, when asked, its run report to `run_report_path`.
struct RemoteVideoRequest {
  // Names the video in the batch ([A-Za-z0-9._-]).
  std::string item_id;
  VideoBuildParameters parameters;
  std::filesystem::path source_path;
  std::filesystem::path output_path;
  std::filesystem::path run_report_path;
};

// One other Mac that coordinates whole-video jobs for this batch.
class RemoteVideoBuilder {
 public:
  virtual ~RemoteVideoBuilder() = default;
  // For reports: its pairing id and how the user named it.
  [[nodiscard]] virtual std::string name() const = 0;
  // Called from one thread at a time per Mac. Never throws.
  [[nodiscard]] virtual RemoteVideoOutcome build(const RemoteVideoRequest& request) = 0;
};

}  // namespace svp::builder::batch
