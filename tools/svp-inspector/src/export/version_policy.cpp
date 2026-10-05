#include "version_policy.hpp"

#include "svp/package/embedded_svpi_transport_profile.hpp"
#include "svp/package/media_binding.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <type_traits>

namespace package_export {
namespace {

// The SVP package format this build's validator implements and this build's
// builder writes into `manifest.json` (and into SVPI manifests). A package
// declaring any other value may use layer semantics this build does not know,
// so the export refuses it rather than exporting it as if it were RC2.
constexpr std::array<std::string_view, 1> kSupportedSvpVersions{"1.0-rc.2"};

// SVPI sidecar versions this build reads and writes.
constexpr std::array<std::string_view, 1> kSupportedSvpiVersions{
    svp::package::kSvpiVersion};

// Embedded SVPI Transport profile versions this build implements.
constexpr std::array<std::uint16_t, 1> kSupportedTransportProfileVersions{
    svp::package::kEmbeddedSvpiProfileVersion};

template <typename Values>
nlohmann::json to_json_array(const Values& values) {
  auto result = nlohmann::json::array();
  for (const auto& value : values) {
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>,
                                 std::string_view>) {
      result.push_back(std::string{value});
    } else {
      result.push_back(value);
    }
  }
  return result;
}

bool is_supported_string(const nlohmann::json& declared,
                         const auto& supported) {
  if (!declared.is_string()) {
    return false;
  }
  const auto& value = declared.get_ref<const std::string&>();
  return std::ranges::any_of(
      supported, [&](std::string_view candidate) { return candidate == value; });
}

bool is_supported_profile(const nlohmann::json& declared) {
  if (!declared.is_number_unsigned()) {
    return false;
  }
  const auto value = declared.get<std::uint64_t>();
  return std::ranges::any_of(kSupportedTransportProfileVersions,
                             [&](std::uint16_t candidate) {
                               return candidate == value;
                             });
}

VersionProblem make_problem(std::string field, const nlohmann::json& declared,
                            nlohmann::json supported) {
  std::string message =
      declared.is_null()
          ? "Package does not declare " + field + ", which this build requires."
          : "Package declares " + field + " " + declared.dump() +
                ", which this build does not support.";
  return VersionProblem{
      .field = std::move(field),
      .declared = declared,
      .supported = std::move(supported),
      .message = std::move(message),
  };
}

nlohmann::json member_or_null(const nlohmann::json& object,
                              std::string_view name) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto found = object.find(std::string{name});
  return found == object.end() ? nlohmann::json(nullptr) : *found;
}

bool requires_svpi_version(PackageKind kind) noexcept {
  return kind == PackageKind::svpi || kind == PackageKind::embedded_svpi;
}

}  // namespace

DeclaredVersions declared_versions(
    const nlohmann::json& manifest, PackageKind kind,
    std::optional<std::uint16_t> transport_profile_version) {
  DeclaredVersions versions;
  versions.svp_version = member_or_null(manifest, "svp_version");
  if (requires_svpi_version(kind)) {
    versions.svpi_version = member_or_null(manifest, "svpi_version");
  }
  if (kind == PackageKind::embedded_svpi &&
      transport_profile_version.has_value()) {
    versions.embedded_transport_profile_version = *transport_profile_version;
  }
  return versions;
}

std::optional<VersionProblem> find_unsupported_declared_version(
    const DeclaredVersions& versions, PackageKind kind) {
  if (!versions.embedded_transport_profile_version.is_null() &&
      !is_supported_profile(versions.embedded_transport_profile_version)) {
    return make_problem("embedded_transport_profile_version",
                        versions.embedded_transport_profile_version,
                        supported_transport_profile_versions());
  }
  if (requires_svpi_version(kind) && !versions.svpi_version.is_null() &&
      !is_supported_string(versions.svpi_version, kSupportedSvpiVersions)) {
    return make_problem("svpi_version", versions.svpi_version,
                        to_json_array(kSupportedSvpiVersions));
  }
  if (!versions.svp_version.is_null() &&
      !is_supported_string(versions.svp_version, kSupportedSvpVersions)) {
    return make_problem("svp_version", versions.svp_version,
                        to_json_array(kSupportedSvpVersions));
  }
  return std::nullopt;
}

std::optional<VersionProblem> find_missing_required_version(
    const DeclaredVersions& versions, PackageKind kind) {
  if (kind == PackageKind::embedded_svpi &&
      versions.embedded_transport_profile_version.is_null()) {
    return make_problem("embedded_transport_profile_version", nullptr,
                        supported_transport_profile_versions());
  }
  if (requires_svpi_version(kind) && versions.svpi_version.is_null()) {
    return make_problem("svpi_version", nullptr,
                        to_json_array(kSupportedSvpiVersions));
  }
  if (kind == PackageKind::svp && versions.svp_version.is_null()) {
    return make_problem("svp_version", nullptr,
                        to_json_array(kSupportedSvpVersions));
  }
  return std::nullopt;
}

nlohmann::json supported_transport_profile_versions() {
  return to_json_array(kSupportedTransportProfileVersions);
}

nlohmann::json to_json(const DeclaredVersions& versions) {
  return nlohmann::json{
      {"svp_version", versions.svp_version},
      {"svpi_version", versions.svpi_version},
      {"embedded_transport_profile_version",
       versions.embedded_transport_profile_version},
  };
}

}  // namespace package_export
