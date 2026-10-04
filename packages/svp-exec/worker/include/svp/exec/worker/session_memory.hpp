#pragma once

// The memory each coordinator session's process holds right now, so memory
// admission (admission.hpp) can tell how much of a running lease's estimate
// is already in use, and therefore already missing from the worker's
// available memory, and how much is still to come.
//
// The figure is the process's resident size: the same measure the task
// estimates (TaskResources::est_peak_rss_mb) are taken in, so the two can be
// compared directly. One session process runs every lease of its session,
// so the figure belongs to the session, not to one lease.
//
// A session whose process is not known, has exited, or cannot be read has
// no figure (nullopt); admission then counts its estimates in full.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace svp::exec::worker {

// Resident bytes of process `pid` (proc_pid_rusage ri_resident_size), or
// nullopt when it cannot be read (no such process, not ours).
[[nodiscard]] std::optional<std::uint64_t> process_resident_bytes(pid_t pid);

// Thread-safe map from worker session id to the pid of its session process.
class SessionMemory {
 public:
  using Probe = std::function<std::optional<std::uint64_t>(pid_t)>;

  // Empty `probe`: process_resident_bytes.
  explicit SessionMemory(Probe probe = {});

  // Records the session's process; replaces an earlier one.
  void attach(std::string_view session, pid_t pid);
  // Forgets the session (its process has exited or is about to). Idempotent.
  void detach(std::string_view session);

  // The session's process's memory now, or nullopt (see above).
  [[nodiscard]] std::optional<std::uint64_t> in_use(std::string_view session) const;

 private:
  Probe probe_;
  mutable std::mutex mutex_;
  std::map<std::string, pid_t, std::less<>> pids_;
};

}  // namespace svp::exec::worker
