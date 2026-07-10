#include "package_source.hpp"

#include "svp/core/path.hpp"
#include "svp/package/embedded_svpi.hpp"

namespace svp::package::detail {

PackageByteRangeResult resolve_package_byte_range(
    const std::filesystem::path& path) {
  if (!svp::core::has_extension(path, ".mp4")) {
    return {.success = true, .range = {}};
  }

  const auto inspection = inspect_embedded_svpi(path, false);
  if (!inspection.has_single_valid_embedding()) {
    std::string message = "MP4 does not contain one valid embedded SVPI";
    if (!inspection.issues.empty()) {
      message += ": " + inspection.issues.front().message;
    }
    return {.success = false, .error_message = std::move(message)};
  }

  const auto& embedding = inspection.embeddings.front();
  return {
      .success = true,
      .range = {
          .offset = embedding.payload_offset,
          .size = embedding.payload_size,
          .bounded = true,
      },
  };
}

}  // namespace svp::package::detail
