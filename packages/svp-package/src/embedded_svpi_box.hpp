#pragma once

#include <cstdint>
#include <vector>

namespace svp::package::detail {

[[nodiscard]] std::vector<std::uint8_t> make_svpi_uuid_box_header(
    std::uint64_t svpi_payload_size);
[[nodiscard]] std::vector<std::uint8_t> make_svpi_uuid_box_header(
    std::uint64_t svpi_payload_size,
    std::uint64_t compact_box_size_limit);

}  // namespace svp::package::detail
