#include "package_source.hpp"

#include "svp/package/embedded_package_session.hpp"
#include "svp/package/embedded_svpi.hpp"

namespace svp::package::detail {

PackageByteRangeResult resolve_package_byte_range(
    const std::filesystem::path& path) {
  std::uint64_t verified_offset = 0;
  std::uint64_t verified_size = 0;
  if (VerifiedEmbeddedPackageSession::active_range_for(
          path, verified_offset, verified_size)) {
    return {
        .success = true,
        .range = {
            .offset = verified_offset,
            .size = verified_size,
            .bounded = true,
        },
    };
  }
  const auto inspection = inspect_embedded_svpi(path, true);
  if (!inspection.container.signature_present) {
    return {.success = true, .range = {}};
  }
  if (!inspection.container.structure_valid ||
      !inspection.container.supported) {
    return {
        .success = false,
        .error_message = inspection.container.diagnostic,
    };
  }

  if (!inspection.has_single_valid_embedding() ||
      !inspection.embeddings.front().hash_verified ||
      !inspection.embeddings.front().hash_matches) {
    std::string message =
        "ISO BMFF container does not contain one valid embedded SVPI";
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
