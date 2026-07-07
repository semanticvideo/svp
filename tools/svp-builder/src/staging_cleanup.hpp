#pragma once

#include <filesystem>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <csignal>
#include <pthread.h>

namespace svp::builder {

namespace staging_cleanup_internal {

struct StagingRegistry {
  std::mutex mutex;
  std::vector<std::filesystem::path> paths;
};

inline StagingRegistry& registry() {
  static StagingRegistry* instance = new StagingRegistry();
  return *instance;
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
  StagingRegistry& staging_registry = registry();
  std::lock_guard<std::mutex> lock(staging_registry.mutex);
  staging_registry.paths.push_back(path);
}

inline void unregister_auto_staging(const std::filesystem::path& path) {
  StagingRegistry& staging_registry = registry();
  std::lock_guard<std::mutex> lock(staging_registry.mutex);
  auto& paths = staging_registry.paths;
  paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
}

inline void cleanup_registered_auto_staging() noexcept {
  std::vector<std::filesystem::path> paths;
  {
    StagingRegistry& staging_registry = registry();
    std::lock_guard<std::mutex> lock(staging_registry.mutex);
    paths = staging_registry.paths;
    staging_registry.paths.clear();
  }
  for (const auto& path : paths) {
    remove_staging_path(path);
  }
}

}  // namespace staging_cleanup_internal

inline void install_staging_interrupt_cleanup() {
  static const bool installed = [] {
    std::atexit(staging_cleanup_internal::cleanup_registered_auto_staging);

    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) != 0) {
      return true;
    }

    std::thread([signal_set] {
      int signal_number = 0;
      if (sigwait(&signal_set, &signal_number) == 0) {
        staging_cleanup_internal::cleanup_registered_auto_staging();
        std::_Exit(128 + signal_number);
      }
    }).detach();
    return true;
  }();
  (void)installed;
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
