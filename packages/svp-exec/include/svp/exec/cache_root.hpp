#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace svp::exec {

// Environment override named by RC2 §20.3 for development, CI, and render-farm
// operation.
inline constexpr const char* kCacheDirEnvironmentVariable = "SVP_CACHE_DIR";

// `xdg` covers Linux and other XDG Base Directory platforms. (`linux` itself
// is a predefined macro under GNU C++ dialects.)
enum class CachePlatform { macos, xdg, windows };

[[nodiscard]] CachePlatform current_cache_platform() noexcept;

// The environment values RC2 §20.3 consults. Unset variables are nullopt; an
// empty value counts as unset.
struct CacheRootEnvironment {
  std::optional<std::string> svp_cache_dir;
  std::optional<std::string> home;
  std::optional<std::string> xdg_cache_home;
  std::optional<std::string> local_app_data;
};

[[nodiscard]] CacheRootEnvironment read_cache_root_environment();

// RC2 §20.3 default roots, after the SVP_CACHE_DIR override:
//   macOS:   ~/Library/Caches/org.svp/cache/v1
//   Linux (xdg): ${XDG_CACHE_HOME}/svp/v1, or ~/.cache/svp/v1 when unset
//   Windows: %LOCALAPPDATA%\SVP\Cache\v1
// nullopt when the variables the platform needs are missing.
[[nodiscard]] std::optional<std::filesystem::path> cache_root_for(
    CachePlatform platform, const CacheRootEnvironment& environment);

// cache_root_for(current_cache_platform(), read_cache_root_environment()).
[[nodiscard]] std::optional<std::filesystem::path> default_cache_root();

}  // namespace svp::exec
