#pragma once

// What a Mac's own --distributed build runs right now (M6), for the worker
// agent on the same Mac: a Mac can coordinate a build and serve other
// coordinators at once, and its agent must not hand other coordinators the
// slots its own build is using (slot_sharing.hpp).
//
// Each build process keeps one file while it runs:
//
//   <SVP support>/LocalLoad/<pid>.json
//     {"pid":n,"running":{"<task_type>":n,...},"schema":"svp.local-load/1"}
//
// (<SVP support> is the parent of the coordinator pairings directory,
// pairing_store.hpp, so SVP_PAIRINGS_DIR relocates it too; the worker agent
// runs as the same user, with that user's HOME, in both launchd modes.)
// `running` counts the build's own in-process tasks per type, and
// kCoordinatingTaskType while it coordinates a video at all, so a Mac busy
// with its own video takes no whole-video job from another Mac.
//
// The agent sums every file whose process is still alive; files of a process
// that died without removing its file (a crash, SIGKILL) are ignored, and
// removed by the next build that writes into the directory. A plain local
// build (no --distributed) keeps no file.

#include "svp/exec/worker/slot_sharing.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace svp::exec::worker {

inline constexpr std::string_view kLocalLoadSchema = "svp.local-load/1";

// The whole-video job type (video.build): a coordinating Mac counts as
// running one while its build lasts.
inline constexpr std::string_view kCoordinatingTaskType = "video.build";

// <parent of default_coordinator_pairings_dir()>/LocalLoad.
[[nodiscard]] std::filesystem::path default_local_load_dir();

// The running counts of every live build under `directory`, summed. A
// missing directory, an unreadable or malformed file, or a dead process's
// file contributes nothing.
[[nodiscard]] TaskTypeCounts read_local_load(const std::filesystem::path& directory);

// One build process's file. Thread-safe; every change rewrites the file
// atomically (temp file, rename) so the agent never reads half a record.
// Writing is best effort: a file that cannot be written leaves the agent
// unaware of this build, never fails the build.
class LocalLoadRecorder {
 public:
  explicit LocalLoadRecorder(std::filesystem::path directory = default_local_load_dir());
  ~LocalLoadRecorder();
  LocalLoadRecorder(const LocalLoadRecorder&) = delete;
  LocalLoadRecorder& operator=(const LocalLoadRecorder&) = delete;

  void add(std::string_view task_type);
  void remove(std::string_view task_type);

  [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

 private:
  void write_locked();

  std::filesystem::path directory_;
  std::filesystem::path file_;
  std::mutex mutex_;
  TaskTypeCounts running_;
};

}  // namespace svp::exec::worker
