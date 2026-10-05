#pragma once

#include "package_kind.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace package_export {

// The format versions a package declares. Each value is the JSON value the
// package wrote (any JSON type), or null when it declares none.
struct DeclaredVersions {
  nlohmann::json svp_version;
  nlohmann::json svpi_version;
  nlohmann::json embedded_transport_profile_version;
};

[[nodiscard]] DeclaredVersions declared_versions(
    const nlohmann::json& manifest, PackageKind kind,
    std::optional<std::uint16_t> transport_profile_version);

// One version the export cannot accept.
struct VersionProblem {
  std::string field;
  nlohmann::json declared;
  nlohmann::json supported;
  std::string message;
};

// A version that is declared but not supported by this build.
[[nodiscard]] std::optional<VersionProblem> find_unsupported_declared_version(
    const DeclaredVersions& versions, PackageKind kind);

// A version this package form must declare but does not.
[[nodiscard]] std::optional<VersionProblem> find_missing_required_version(
    const DeclaredVersions& versions, PackageKind kind);

// The supported values of one versioned field, for error details.
[[nodiscard]] nlohmann::json supported_transport_profile_versions();

[[nodiscard]] nlohmann::json to_json(const DeclaredVersions& versions);

}  // namespace package_export
