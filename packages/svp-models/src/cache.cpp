#include "svp/models/cache.hpp"

#include <cstdlib>
#include <string>

namespace svp::models {
namespace {

std::filesystem::path home_directory() {
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home);
  }

#if defined(_WIN32)
  if (const char* profile = std::getenv("USERPROFILE");
      profile != nullptr && profile[0] != '\0') {
    return std::filesystem::path(profile);
  }
#endif

  return {};
}

}  // namespace

std::filesystem::path default_cache_root() {
  if (const char* override_root = std::getenv("SVP_CACHE_DIR");
      override_root != nullptr && override_root[0] != '\0') {
    return std::filesystem::path(override_root);
  }

#if defined(_WIN32)
  if (const char* local_app_data = std::getenv("LOCALAPPDATA");
      local_app_data != nullptr && local_app_data[0] != '\0') {
    return std::filesystem::path(local_app_data) / "SVP" / "Cache" / "v1";
  }
  return home_directory() / "AppData" / "Local" / "SVP" / "Cache" / "v1";
#elif defined(__APPLE__)
  return home_directory() / "Library" / "Caches" / "org.svp" / "cache" / "v1";
#else
  if (const char* xdg_cache_home = std::getenv("XDG_CACHE_HOME");
      xdg_cache_home != nullptr && xdg_cache_home[0] != '\0') {
    return std::filesystem::path(xdg_cache_home) / "svp" / "v1";
  }
  return home_directory() / ".cache" / "svp" / "v1";
#endif
}

std::filesystem::path model_cache_root() {
  return default_cache_root() / "models";
}

}  // namespace svp::models
