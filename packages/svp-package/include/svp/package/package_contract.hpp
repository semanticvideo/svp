#pragma once

#include <span>
#include <string_view>

namespace svp::package {

enum class PackageLayoutRequirementKind {
  required_entry,
  required_top_level_section,
};

enum class PackageLayoutRequirementArea {
  core,
  text,
  colors,
};

struct PackageLayoutRequirement {
  std::string_view path;
  PackageLayoutRequirementKind kind = PackageLayoutRequirementKind::required_entry;
  PackageLayoutRequirementArea area = PackageLayoutRequirementArea::core;
};

[[nodiscard]] std::span<const PackageLayoutRequirement> required_package_layout();
[[nodiscard]] bool is_allowed_root_entry(std::string_view root_entry) noexcept;

}  // namespace svp::package
