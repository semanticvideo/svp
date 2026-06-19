#pragma once

#include "svp/core/hash_string.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace svp::models {

struct ModelBundleIdParts {
  std::string model_id;
  std::string model_version;
  std::string hash_prefix;
};

[[nodiscard]] bool is_canonical_model_id(std::string_view value) noexcept;
[[nodiscard]] std::optional<ModelBundleIdParts> parse_model_bundle_id(
    std::string_view value);
[[nodiscard]] bool is_canonical_model_bundle_id(std::string_view value);
[[nodiscard]] std::string expected_model_bundle_id(
    std::string_view model_id,
    std::string_view model_version,
    const svp::core::HashString& bundle_blake3);

}  // namespace svp::models
