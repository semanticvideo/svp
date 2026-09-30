#include "svp/package/output_directory.hpp"

namespace svp::package {

void ensure_parent_directory(const std::filesystem::path& output_path) {
  const std::filesystem::path parent = output_path.parent_path();
  if (parent.empty()) return;
  std::filesystem::create_directories(parent);
}

}  // namespace svp::package
