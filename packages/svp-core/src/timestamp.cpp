#include "svp/core/timestamp.hpp"

namespace svp::core {

bool is_valid(RationalTime value) noexcept {
  return value.denominator > 0;
}

}  // namespace svp::core

