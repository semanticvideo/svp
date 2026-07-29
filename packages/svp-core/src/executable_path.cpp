#include "svp/core/executable_path.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace svp::core {
namespace {

std::filesystem::path native_executable() {
#ifdef __APPLE__
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    return resolve_executable(buffer.data());
  }
#elif defined(__linux__)
  std::vector<char> buffer(1024);
  while (true) {
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) {
      return {};
    }
    if (static_cast<std::size_t>(length) < buffer.size()) {
      return resolve_executable(
          std::string(buffer.data(), static_cast<std::size_t>(length)));
    }
    buffer.resize(buffer.size() * 2);
  }
#endif
  return {};
}

std::filesystem::path invoked_executable(
    const std::filesystem::path& invoked_path) {
  if (invoked_path.empty()) {
    return {};
  }

  if (invoked_path.has_parent_path()) {
    return resolve_executable(invoked_path);
  }

  const char* path_value = std::getenv("PATH");
  if (path_value == nullptr) {
    return {};
  }

  const std::string paths(path_value);
  std::size_t begin = 0;
  while (begin <= paths.size()) {
    const std::size_t end = paths.find(':', begin);
    const std::string directory = paths.substr(begin, end - begin);
    const auto candidate =
        (directory.empty() ? std::filesystem::path(".")
                           : std::filesystem::path(directory)) /
        invoked_path;
    if (const auto resolved = resolve_executable(candidate); !resolved.empty()) {
      return resolved;
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return {};
}

}  // namespace

std::filesystem::path resolve_executable(
    const std::filesystem::path& candidate) {
  if (candidate.empty()) {
    return {};
  }

  std::error_code error;
  const auto resolved = std::filesystem::canonical(candidate, error);
  if (error || !std::filesystem::is_regular_file(resolved, error) || error ||
      access(resolved.c_str(), X_OK) != 0) {
    return {};
  }
  return resolved;
}

std::filesystem::path resolve_current_executable(
    const std::filesystem::path& invoked_path) {
  if (const auto resolved = native_executable(); !resolved.empty()) {
    return resolved;
  }
  if (const auto resolved = invoked_executable(invoked_path); !resolved.empty()) {
    return resolved;
  }
  throw std::runtime_error("could not resolve the running executable");
}

}  // namespace svp::core
