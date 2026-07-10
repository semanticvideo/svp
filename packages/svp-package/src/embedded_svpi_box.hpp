#pragma once

#include <cstdint>
#include <vector>

namespace svp::package::detail {

[[nodiscard]] std::vector<std::uint8_t> make_svpi_uuid_box_header(
    std::uint64_t svpi_payload_size);

}  // namespace svp::package::detail
