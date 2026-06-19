#pragma once

#include <filesystem>
#include <span>
#include <string_view>

namespace svp::validation {

enum class SpecAssetKind {
  registry,
  schema,
};

struct SpecAsset {
  std::string_view relative_path;
  SpecAssetKind kind = SpecAssetKind::registry;
};

[[nodiscard]] std::span<const SpecAsset> required_rc2_spec_assets() noexcept;
void load_json_spec_asset(const std::filesystem::path& path);

}  // namespace svp::validation
