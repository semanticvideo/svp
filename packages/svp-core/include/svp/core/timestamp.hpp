#pragma once

#include <cstdint>

namespace svp::core {

struct RationalTime {
  std::int64_t numerator = 0;
  std::int64_t denominator = 1;
};

struct Timestamp {
  std::int64_t microseconds = 0;
};

[[nodiscard]] bool is_valid(RationalTime value) noexcept;

}  // namespace svp::core

