#include "svp/package/package_contract.hpp"

#include <array>

namespace svp::package {
namespace {

constexpr std::array<PackageLayoutRequirement, 20> kRequiredPackageLayout{{
    {"manifest.json", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::core},
    {"mimetype", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::core},
    {"media", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
    {"transcript", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
    {"timeline", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
    {"entities", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
    {"spatial", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
    {"text", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::text},
    {"text/text_regions.jsonl", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::text},
    {"text/text_observations.jsonl", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::text},
    {"text/numeric_values.jsonl", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::text},
    {"text/text_absence.json", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::text},
    {"colors", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::colors},
    {"colors/color_observations.jsonl", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::colors},
    {"colors/color_summary.json", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::colors},
    {"colors/color_absence.json", PackageLayoutRequirementKind::required_entry,
     PackageLayoutRequirementArea::colors},
    {"relationships", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
    {"embeddings", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
    {"index", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
    {"provenance", PackageLayoutRequirementKind::required_top_level_section,
     PackageLayoutRequirementArea::core},
}};

constexpr std::array<std::string_view, 1> kAdditionalAllowedRootEntries{
    "labels",
};

}  // namespace

std::span<const PackageLayoutRequirement> required_package_layout() {
  return kRequiredPackageLayout;
}

bool is_allowed_root_entry(std::string_view root_entry) noexcept {
  for (const auto& requirement : kRequiredPackageLayout) {
    const auto slash = requirement.path.find('/');
    const auto requirement_root =
        slash == std::string_view::npos ? requirement.path : requirement.path.substr(0, slash);
    if (root_entry == requirement_root) {
      return true;
    }
  }

  for (const auto allowed_root : kAdditionalAllowedRootEntries) {
    if (root_entry == allowed_root) {
      return true;
    }
  }

  return false;
}

}  // namespace svp::package
