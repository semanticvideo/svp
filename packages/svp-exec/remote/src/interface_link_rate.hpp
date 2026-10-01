#pragma once

#include <cstdint>
#include <string_view>

namespace svp::exec::remote::detail {

// Link rate the named interface reports (if_data.ifi_baudrate), in bits per
// second; 0 when the interface is unknown or reports none. The kernel field
// saturates at 2^32-1 on this platform, so every link at or above ~4.3 Gb/s
// (Thunderbolt, 10 GbE) reports the same value; ties are then broken by
// measured latency (route_policy.hpp).
[[nodiscard]] std::uint64_t interface_link_rate_bps(std::string_view interface_name);

}  // namespace svp::exec::remote::detail
