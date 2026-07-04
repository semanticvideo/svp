#pragma once

#include <filesystem>
#include <iostream>
#include <system_error>

namespace svp::builder {

class StagingCleanupGuard {
 public:
  StagingCleanupGuard(const std::filesystem::path& path, bool user_supplied)
      : path_(path), user_supplied_(user_supplied) {}

  ~StagingCleanupGuard() {
    if (user_supplied_ || cleaned_up_) return;
    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) {
      std::cerr << "Staging preserved for debugging: " << path_.string() << "\n";
    }
  }

  StagingCleanupGuard(const StagingCleanupGuard&) = delete;
  StagingCleanupGuard& operator=(const StagingCleanupGuard&) = delete;

  void cleanup_on_success() {
    cleaned_up_ = true;
    if (user_supplied_) return;
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    if (ec) {
      std::cerr << "svp-builder: warning: failed to remove staging directory: "
                << ec.message() << "\n";
    }
  }

  void release() { cleaned_up_ = true; }

 private:
  std::filesystem::path path_;
  bool user_supplied_;
  bool cleaned_up_ = false;
};

}  // namespace svp::builder
