#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace svp::media {

struct Rational {
  std::int64_t numerator = 0;
  std::int64_t denominator = 1;
};

[[nodiscard]] bool is_valid(Rational value) noexcept;
[[nodiscard]] Rational normalize(Rational value);
[[nodiscard]] std::optional<Rational> parse_rational(std::string_view value);
[[nodiscard]] std::string format_rational(Rational value);

}  // namespace svp::media
