#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace svp::package::detail {

struct PackageByteRange {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  bool bounded = false;
};

struct PackageByteRangeResult {
  bool success = false;
  PackageByteRange range;
  std::string error_message;
};

[[nodiscard]] PackageByteRangeResult resolve_package_byte_range(
    const std::filesystem::path& path);

}  // namespace svp::package::detail
