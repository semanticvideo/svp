#include "svp/exec/cache_root.hpp"

#include <cstdlib>

namespace svp::exec {
namespace {

std::optional<std::string> environment_value(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

bool present(const std::optional<std::string>& value) {
  return value.has_value() && !value->empty();
}

}  // namespace

CachePlatform current_cache_platform() noexcept {
#if defined(_WIN32)
  return CachePlatform::windows;
#elif defined(__APPLE__)
  return CachePlatform::macos;
#else
  return CachePlatform::xdg;
#endif
}

CacheRootEnvironment read_cache_root_environment() {
  return CacheRootEnvironment{.svp_cache_dir = environment_value(kCacheDirEnvironmentVariable),
                              .home = environment_value("HOME"),
                              .xdg_cache_home = environment_value("XDG_CACHE_HOME"),
                              .local_app_data = environment_value("LOCALAPPDATA")};
}

std::optional<std::filesystem::path> cache_root_for(CachePlatform platform,
                                                    const CacheRootEnvironment& environment) {
  namespace fs = std::filesystem;
  if (present(environment.svp_cache_dir)) {
    return fs::path(*environment.svp_cache_dir);
  }
  switch (platform) {
    case CachePlatform::macos:
      if (present(environment.home)) {
        return fs::path(*environment.home) / "Library" / "Caches" / "org.svp" / "cache" / "v1";
      }
      return std::nullopt;
    case CachePlatform::xdg:
      // XDG Base Directory: a relative XDG_CACHE_HOME is invalid and ignored.
      if (present(environment.xdg_cache_home) &&
          fs::path(*environment.xdg_cache_home).is_absolute()) {
        return fs::path(*environment.xdg_cache_home) / "svp" / "v1";
      }
      if (present(environment.home)) {
        return fs::path(*environment.home) / ".cache" / "svp" / "v1";
      }
      return std::nullopt;
    case CachePlatform::windows:
      if (present(environment.local_app_data)) {
        return fs::path(*environment.local_app_data) / "SVP" / "Cache" / "v1";
      }
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> default_cache_root() {
  return cache_root_for(current_cache_platform(), read_cache_root_environment());
}

}  // namespace svp::exec
