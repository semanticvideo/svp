#include "svp/exec/worker/service_link.hpp"

#include "svp/exec/worker/worker_error.hpp"

#include <cerrno>
#include <cstring>
#include <stdio.h>
#include <unistd.h>

namespace svp::exec::worker {
namespace {

constexpr std::string_view kRuntimesDirName = "runtimes";

}  // namespace

std::string current_link_target(const Blake3Digest& runtime_id) {
  return std::string(kRuntimesDirName) + "/" + blake3_hex(runtime_id);
}

std::optional<Blake3Digest> read_current_runtime(const WorkerLayout& layout) {
  std::error_code error;
  if (!std::filesystem::is_symlink(layout.current(), error)) {
    return std::nullopt;
  }
  const std::filesystem::path target = std::filesystem::read_symlink(layout.current(), error);
  if (error) {
    return std::nullopt;
  }
  const std::string text = target.generic_string();
  const std::string prefix = std::string(kRuntimesDirName) + "/";
  if (!text.starts_with(prefix)) {
    return std::nullopt;
  }
  return parse_blake3_hex(std::string_view(text).substr(prefix.size()));
}

void point_current_at(const WorkerLayout& layout, const Blake3Digest& runtime_id) {
  const std::filesystem::path temporary =
      layout.root / (".current-" + std::to_string(::getpid()));
  const std::string target = current_link_target(runtime_id);
  ::unlink(temporary.c_str());
  if (::symlink(target.c_str(), temporary.c_str()) != 0) {
    throw WorkerError(WorkerErrorCode::io, "cannot create " + temporary.string() + ": " +
                                               std::strerror(errno));
  }
  // rename(2) replaces the old link itself (it never follows a symlink
  // destination), atomically.
  if (::rename(temporary.c_str(), layout.current().c_str()) != 0) {
    const int failure = errno;
    ::unlink(temporary.c_str());
    throw WorkerError(WorkerErrorCode::io, "cannot point " + layout.current().string() +
                                               " at " + target + ": " + std::strerror(failure));
  }
}

bool launched_through_current(const WorkerLayout& layout,
                              const std::filesystem::path& launched_program) {
  return launched_program.lexically_normal() == layout.service_program().lexically_normal();
}

}  // namespace svp::exec::worker
