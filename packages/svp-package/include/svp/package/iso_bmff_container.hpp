#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svp::package {

enum class IsoBmffContainerKind {
  unknown,
  mp4,
  quicktime,
  m4v,
  m4a,
  unsupported_derivative,
};

struct IsoBmffContainerInfo {
  bool signature_present = false;
  bool structure_valid = false;
  bool supported = false;
  std::uint64_t bytes_read = 0;
  IsoBmffContainerKind kind = IsoBmffContainerKind::unknown;
  std::string major_brand;
  std::uint32_t minor_version = 0;
  std::vector<std::string> compatible_brands;
  std::string diagnostic;
};

[[nodiscard]] IsoBmffContainerInfo inspect_iso_bmff_container(
    const std::filesystem::path& path);
[[nodiscard]] const char* to_string(IsoBmffContainerKind kind) noexcept;

}  // namespace svp::package
