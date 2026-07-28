#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <vector>

namespace svp::models::detail {

[[nodiscard]] std::vector<std::uint8_t> encode_canonical_control_cbor(
    const nlohmann::json& value);

}  // namespace svp::models::detail
