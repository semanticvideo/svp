#include "svp/vision/tasks/ffmpeg_build_identity.hpp"

#include "svp/exec/blake3_digest.hpp"

#include <array>
#include <cstdio>
#include <map>
#include <mutex>
#include <sys/wait.h>

namespace svp::vision::tasks {
namespace {

std::string shell_quoted(const std::string& text) {
  std::string quoted = "'";
  for (const char character : text) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  return quoted + "'";
}

}  // namespace

std::optional<std::string> ffmpeg_build_identity(const std::filesystem::path& ffmpeg) {
  if (ffmpeg.empty()) {
    return std::nullopt;
  }
  // stderr is discarded: `-version` prints everything on stdout, and a
  // missing program's shell message must not become part of an identity.
  const std::string command = shell_quoted(ffmpeg.string()) + " -version 2>/dev/null";
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return std::nullopt;
  }
  std::string output;
  std::array<char, 4096> buffer{};
  while (const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
    output.append(buffer.data(), read);
  }
  const int status = ::pclose(pipe);
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || output.empty()) {
    return std::nullopt;
  }
  return svp::exec::blake3_prefixed(svp::exec::blake3_digest(output));
}

std::optional<std::string> cached_ffmpeg_build_identity(const std::filesystem::path& ffmpeg) {
  static std::mutex mutex;
  // Only successes are kept: a failure may be transient (popen under load),
  // so the next call asks ffmpeg again.
  static std::map<std::string, std::string> cache;
  {
    const std::lock_guard lock(mutex);
    if (const auto found = cache.find(ffmpeg.string()); found != cache.end()) {
      return found->second;
    }
  }
  std::optional<std::string> identity = ffmpeg_build_identity(ffmpeg);
  if (!identity) {
    return std::nullopt;
  }
  const std::lock_guard lock(mutex);
  return cache.emplace(ffmpeg.string(), std::move(*identity)).first->second;
}

}  // namespace svp::vision::tasks
