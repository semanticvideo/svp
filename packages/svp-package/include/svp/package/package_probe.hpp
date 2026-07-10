#pragma once

#include "svp/package/iso_bmff_container.hpp"

#include <filesystem>

namespace svp::package {

struct PackageProbe {
  std::filesystem::path path;
  bool exists = false;
  bool is_regular_file = false;
  bool has_svp_extension = false;
  bool has_svpi_extension = false;
  IsoBmffContainerInfo iso_bmff;
};

[[nodiscard]] PackageProbe probe_package(const std::filesystem::path& path);

}  // namespace svp::package
