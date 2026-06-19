#include "svp/core/path.hpp"

namespace svp::core {

std::filesystem::path normalize_path(std::filesystem::path path) {
  return path.lexically_normal();
}

bool has_extension(std::filesystem::path path, std::string_view extension) {
  return path.extension().string() == extension;
}

}  // namespace svp::core

