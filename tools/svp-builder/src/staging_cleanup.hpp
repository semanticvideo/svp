#pragma once

#include <filesystem>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <csignal>
#include <cstdlib>
#include <system_error>

namespace svp::builder {

namespace staging_cleanup_internal {

inline std::mutex& registry_mutex() {
  static std::mutex mutex;
  return mutex;
}

inline std::vector<std::filesystem::path>& auto_staging_registry() {
  static std::vector<std::filesystem::path> paths;
  return paths;
}

inline void remove_staging_path(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  if (ec) {
    std::cerr << "svp-builder: warning: failed to remove staging directory: "
              << ec.message() << "\n";
  }
}

inline void register_auto_staging(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  auto_staging_registry().push_back(path);
}

inline void unregister_auto_staging(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  auto& paths = auto_staging_registry();
  paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
}

inline void cleanup_registered_auto_staging() noexcept {
  std::vector<std::filesystem::path> paths;
  {
    std::lock_guard<std::mutex> lock(registry_mutex());
    paths = auto_staging_registry();
    auto_staging_registry().clear();
  }
  for (const auto& path : paths) {
    remove_staging_path(path);
  }
}

inline void interrupt_cleanup_handler(int signal_number) {
  cleanup_registered_auto_staging();
  std::_Exit(128 + signal_number);
}

}  // namespace staging_cleanup_internal

inline void install_staging_interrupt_cleanup() {
  std::signal(SIGINT, staging_cleanup_internal::interrupt_cleanup_handler);
}

class StagingCleanupGuard {
 public:
  StagingCleanupGuard(const std::filesystem::path& path, bool user_supplied)
      : path_(path), user_supplied_(user_supplied) {
    if (!user_supplied_) {
      staging_cleanup_internal::register_auto_staging(path_);
    }
  }

  ~StagingCleanupGuard() {
    if (user_supplied_ || cleaned_up_) {
      return;
    }
    cleanup();
  }

  StagingCleanupGuard(const StagingCleanupGuard&) = delete;
  StagingCleanupGuard& operator=(const StagingCleanupGuard&) = delete;

  void cleanup_on_success() {
    cleanup();
  }

  void release() {
    if (!user_supplied_) {
      staging_cleanup_internal::unregister_auto_staging(path_);
    }
    cleaned_up_ = true;
  }

 private:
  void cleanup() {
    if (cleaned_up_) return;
    cleaned_up_ = true;
    if (user_supplied_) return;
    staging_cleanup_internal::unregister_auto_staging(path_);
    staging_cleanup_internal::remove_staging_path(path_);
  }

 private:
  std::filesystem::path path_;
  bool user_supplied_;
  bool cleaned_up_ = false;
};

}  // namespace svp::builder
