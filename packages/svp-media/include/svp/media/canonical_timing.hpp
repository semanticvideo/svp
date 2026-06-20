#pragma once

#include "svp/media/rational.hpp"

#include <cstdint>
#include <string>

namespace svp::media {

[[nodiscard]] std::int64_t round_half_to_even(std::int64_t numerator,
                                              std::int64_t denominator);
[[nodiscard]] std::int64_t pts_to_microseconds(std::int64_t normalized_pts,
                                               Rational source_timebase);
[[nodiscard]] std::int64_t frame_index_to_microseconds(std::int64_t frame_index,
                                                       Rational frame_rate);
[[nodiscard]] std::string microseconds_to_seconds_string(std::int64_t microseconds);

}  // namespace svp::media
