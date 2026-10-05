#pragma once

#include <filesystem>
#include <string_view>

namespace package_export {

enum class PackageKind {
  svp,
  svpi,
  embedded_svpi,
};

// SVP RC2 Section 5.12: the exact content of an SVP package's `mimetype`.
inline constexpr std::string_view kSvpMimetype = "application/vnd.svp+zip";

[[nodiscard]] std::string_view to_string(PackageKind kind) noexcept;

// Detects the input form from content (Package_Export_v1.md Section 1.1):
// an ISO BMFF signature means Embedded SVPI Transport; a ZIP is classified by
// its `mimetype` entry, falling back to the .svp/.svpi extension when the
// mimetype is missing or unknown so the validator can report the problem.
// Throws ExportError(input_unrecognized) for anything else.
[[nodiscard]] PackageKind detect_package_kind(const std::filesystem::path& path);

}  // namespace package_export
