#include "package_kind.hpp"

#include "export_error.hpp"
#include "package_reader.hpp"

#include "svp/core/path.hpp"
#include "svp/package/iso_bmff_container.hpp"
#include "svp/package/media_binding.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <string>

namespace package_export {
namespace {

// ZIP local-file-header and empty-archive end-of-central-directory signatures
// (PKWARE APPNOTE 4.3.7 and 4.3.16): a ZIP package starts with one of them.
constexpr std::array<std::string_view, 2> kZipLeadingSignatures{
    std::string_view{"PK\x03\x04", 4},
    std::string_view{"PK\x05\x06", 4},
};
constexpr std::size_t kZipSignatureBytes = 4;

bool starts_like_zip(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<char, kZipSignatureBytes> head{};
  if (!input.read(head.data(), head.size())) {
    return false;
  }
  const std::string_view prefix{head.data(), head.size()};
  return std::ranges::any_of(kZipLeadingSignatures,
                             [&](std::string_view signature) {
                               return prefix == signature;
                             });
}

std::optional<PackageKind> kind_from_mimetype(
    const std::filesystem::path& path) {
  // A recognized mimetype is one of these exact strings, so nothing longer
  // than the longest of them needs to be read.
  const auto longest_known_mimetype =
      std::max(kSvpMimetype.size(), svp::package::kSvpiMimetype.size());
  try {
    const PackageReader reader{path, std::nullopt};
    const auto* entry = reader.find_file("mimetype");
    if (entry == nullptr || entry->size_bytes > longest_known_mimetype) {
      return std::nullopt;
    }
    const auto content = reader.read_whole(*entry, longest_known_mimetype);
    if (content == kSvpMimetype) {
      return PackageKind::svp;
    }
    if (content == svp::package::kSvpiMimetype) {
      return PackageKind::svpi;
    }
  } catch (const ExportError&) {
    return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

std::string_view to_string(PackageKind kind) noexcept {
  switch (kind) {
    case PackageKind::svp:
      return "svp";
    case PackageKind::svpi:
      return "svpi";
    case PackageKind::embedded_svpi:
      return "embedded_svpi";
  }
  return "svp";
}

PackageKind detect_package_kind(const std::filesystem::path& path) {
  if (svp::package::inspect_iso_bmff_container(path).signature_present) {
    return PackageKind::embedded_svpi;
  }
  if (starts_like_zip(path)) {
    if (const auto kind = kind_from_mimetype(path)) {
      return *kind;
    }
    if (svp::core::has_extension(path, ".svp")) {
      return PackageKind::svp;
    }
    if (svp::core::has_extension(path, ".svpi")) {
      return PackageKind::svpi;
    }
  }
  throw ExportError(
      ExportErrorCode::input_unrecognized,
      "Input is not an SVP package, an SVPI sidecar, or an ISO BMFF file "
      "carrying an Embedded SVPI Transport.");
}

}  // namespace package_export
