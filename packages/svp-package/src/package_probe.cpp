#include "svp/package/package_probe.hpp"

#include "svp/core/path.hpp"

#include <system_error>

namespace svp::package {

PackageProbe probe_package(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::status(path, error);

  PackageProbe probe;
  probe.path = svp::core::normalize_path(path);
  probe.exists = !error && std::filesystem::exists(status);
  probe.is_regular_file = !error && std::filesystem::is_regular_file(status);
  probe.has_svp_extension = svp::core::has_extension(path, ".svp");
  return probe;
}

}  // namespace svp::package

