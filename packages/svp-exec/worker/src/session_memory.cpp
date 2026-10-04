#include "svp/exec/worker/session_memory.hpp"

#include <libproc.h>
#include <sys/resource.h>

#include <utility>

namespace svp::exec::worker {

std::optional<std::uint64_t> process_resident_bytes(pid_t pid) {
  if (pid <= 0) {
    return std::nullopt;
  }
  rusage_info_v2 info{};
  if (::proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&info)) != 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(info.ri_resident_size);
}

SessionMemory::SessionMemory(Probe probe)
    : probe_(probe ? std::move(probe) : Probe([](pid_t pid) { return process_resident_bytes(pid); })) {}

void SessionMemory::attach(std::string_view session, pid_t pid) {
  const std::lock_guard lock(mutex_);
  pids_.insert_or_assign(std::string(session), pid);
}

void SessionMemory::detach(std::string_view session) {
  const std::lock_guard lock(mutex_);
  if (const auto found = pids_.find(session); found != pids_.end()) {
    pids_.erase(found);
  }
}

std::optional<std::uint64_t> SessionMemory::in_use(std::string_view session) const {
  pid_t pid = -1;
  {
    const std::lock_guard lock(mutex_);
    const auto found = pids_.find(session);
    if (found == pids_.end()) {
      return std::nullopt;
    }
    pid = found->second;
  }
  return probe_(pid);
}

}  // namespace svp::exec::worker
