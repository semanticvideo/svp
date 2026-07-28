#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

namespace svp::models::detail {

[[nodiscard]] nlohmann::json parse_canonical_control_json(
    const std::vector<std::uint8_t>& bytes,
    const std::filesystem::path& path);

}  // namespace svp::models::detail
