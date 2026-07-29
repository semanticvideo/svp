#include "executable_path.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace svp::models::tool {
namespace {

std::filesystem::path canonical_executable(
    const std::filesystem::path& candidate) {
  std::error_code error;
  const auto resolved = std::filesystem::canonical(candidate, error);
  if (error || access(resolved.c_str(), X_OK) != 0) return {};
  return resolved;
}

}  // namespace

std::filesystem::path resolve_current_executable(
    const std::filesystem::path& invoked_path) {
#ifdef __APPLE__
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    if (const auto resolved = canonical_executable(buffer.data()); !resolved.empty()) {
      return resolved;
    }
  }
#endif

  if (invoked_path.has_parent_path()) {
    if (const auto resolved = canonical_executable(invoked_path); !resolved.empty()) {
      return resolved;
    }
  } else if (const char* path_value = std::getenv("PATH")) {
    std::string paths(path_value);
    std::size_t begin = 0;
    while (begin <= paths.size()) {
      const std::size_t end = paths.find(':', begin);
      const std::string directory = paths.substr(begin, end - begin);
      const auto candidate = (directory.empty() ? std::filesystem::path(".")
                                                 : std::filesystem::path(directory)) /
                             invoked_path;
      if (const auto resolved = canonical_executable(candidate); !resolved.empty()) {
        return resolved;
      }
      if (end == std::string::npos) break;
      begin = end + 1;
    }
  }
  throw std::runtime_error("could not resolve the running svp-models-tool executable");
}

}  // namespace svp::models::tool
