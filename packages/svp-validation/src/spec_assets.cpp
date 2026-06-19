#include "spec_assets.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <stdexcept>

namespace svp::validation {
namespace {

constexpr std::array<SpecAsset, 11> kRequiredRc2SpecAssets{{
    {"color-buckets.json", SpecAssetKind::registry},
    {"color-spaces.json", SpecAssetKind::registry},
    {"ocr-observation-types.json", SpecAssetKind::registry},
    {"index-manifest.schema.json", SpecAssetKind::schema},
    {"text-region.schema.json", SpecAssetKind::schema},
    {"text-observation.schema.json", SpecAssetKind::schema},
    {"numeric-value.schema.json", SpecAssetKind::schema},
    {"text-absence.schema.json", SpecAssetKind::schema},
    {"color-observation.schema.json", SpecAssetKind::schema},
    {"color-summary.schema.json", SpecAssetKind::schema},
    {"color-absence.schema.json", SpecAssetKind::schema},
}};

}  // namespace

std::span<const SpecAsset> required_rc2_spec_assets() noexcept {
  return kRequiredRc2SpecAssets;
}

void load_json_spec_asset(const std::filesystem::path& path) {
  std::ifstream input{path};
  if (!input) {
    throw std::runtime_error("could not open JSON asset: " + path.string());
  }

  nlohmann::json json;
  input >> json;
}

}  // namespace svp::validation
