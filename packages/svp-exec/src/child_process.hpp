#pragma once

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

namespace svp::exec::detail {

// A spawned worker process whose stdin and stdout are one end of a stream
// socket pair; `fd` is the parent's end (close-on-exec, SIGPIPE suppressed
// where the platform offers SO_NOSIGPIPE).
struct ChildProcess {
  pid_t pid = -1;
  int fd = -1;
};

// Throws ExecError(invalid_value) with the OS reason when the socket pair or
// the process cannot be created. No other descriptor of this process leaks
// into the child.
[[nodiscard]] ChildProcess spawn_child_process(const std::filesystem::path& executable,
                                               const std::vector<std::string>& arguments);

// SIGKILL, then reap. Safe on a process that already exited but was not
// reaped. Blocks until the process is gone.
void kill_and_reap(pid_t pid);

}  // namespace svp::exec::detail
