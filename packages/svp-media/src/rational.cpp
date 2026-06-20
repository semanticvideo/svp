#include "svp/media/rational.hpp"

#include <charconv>
#include <numeric>
#include <stdexcept>
#include <string>

namespace svp::media {
namespace {

std::optional<std::int64_t> parse_integer(std::string_view value) {
  std::int64_t parsed = 0;
  const char* begin = value.data();
  const char* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

}  // namespace

bool is_valid(Rational value) noexcept {
  return value.denominator != 0;
}

Rational normalize(Rational value) {
  if (!is_valid(value)) {
    throw std::invalid_argument("rational denominator must be non-zero");
  }

  if (value.denominator < 0) {
    value.numerator = -value.numerator;
    value.denominator = -value.denominator;
  }

  const std::int64_t divisor = std::gcd(value.numerator, value.denominator);
  if (divisor != 0) {
    value.numerator /= divisor;
    value.denominator /= divisor;
  }
  return value;
}

std::optional<Rational> parse_rational(std::string_view value) {
  const std::size_t separator = value.find_first_of("/:");
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }

  const std::optional<std::int64_t> numerator = parse_integer(value.substr(0, separator));
  const std::optional<std::int64_t> denominator = parse_integer(value.substr(separator + 1));
  if (!numerator.has_value() || !denominator.has_value() || *denominator == 0) {
    return std::nullopt;
  }

  return normalize(Rational{*numerator, *denominator});
}

std::string format_rational(Rational value) {
  const Rational normalized = normalize(value);
  return std::to_string(normalized.numerator) + "/" + std::to_string(normalized.denominator);
}

}  // namespace svp::media
