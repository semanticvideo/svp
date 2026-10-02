#pragma once

// Where this Mac's dispatched vision tasks write temporary files (crop images
// before they are read back, svp/vision/tasks/dispatched_task_environment
// .hpp): a private directory under the system temporary directory, never
// inside staging (whose contents a stage task captures), removed with this
// object.

#include <filesystem>

namespace svp::builder::engine {

class DispatchScratch {
 public:
  // Throws std::runtime_error when the directory cannot be created.
  DispatchScratch();
  ~DispatchScratch();
  DispatchScratch(const DispatchScratch&) = delete;
  DispatchScratch& operator=(const DispatchScratch&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace svp::builder::engine
