#include "svp/media/canonical_timing.hpp"

#include <limits>
#include <stdexcept>

namespace svp::media {
namespace {

std::int64_t checked_int64(__int128 value) {
  if (value > static_cast<__int128>(std::numeric_limits<std::int64_t>::max()) ||
      value < static_cast<__int128>(std::numeric_limits<std::int64_t>::min())) {
    throw std::overflow_error("canonical timestamp is outside int64 range");
  }
  return static_cast<std::int64_t>(value);
}

std::int64_t round_half_to_even_wide(__int128 numerator, __int128 denominator) {
  if (denominator <= 0) {
    throw std::invalid_argument("rounding denominator must be positive");
  }
  if (numerator < 0) {
    throw std::invalid_argument("canonical timestamp numerator must be non-negative");
  }

  const __int128 quotient = numerator / denominator;
  const __int128 remainder = numerator % denominator;
  const __int128 doubled_remainder = remainder * 2;

  __int128 rounded = quotient;
  if (doubled_remainder > denominator ||
      (doubled_remainder == denominator && (quotient % 2) != 0)) {
    ++rounded;
  }

  return checked_int64(rounded);
}

}  // namespace

std::int64_t round_half_to_even(std::int64_t numerator, std::int64_t denominator) {
  return round_half_to_even_wide(static_cast<__int128>(numerator),
                                 static_cast<__int128>(denominator));
}

std::int64_t pts_to_microseconds(std::int64_t normalized_pts,
                                 Rational source_timebase) {
  const Rational timebase = normalize(source_timebase);
  if (normalized_pts < 0) {
    throw std::invalid_argument("normalized source PTS must be non-negative");
  }

  const __int128 numerator = static_cast<__int128>(normalized_pts) *
                             static_cast<__int128>(timebase.numerator) * 1000000;
  return round_half_to_even_wide(numerator, static_cast<__int128>(timebase.denominator));
}

std::int64_t normalized_pts_to_microseconds(
    std::int64_t source_pts,
    Rational source_timebase,
    std::int64_t origin_pts,
    Rational origin_timebase) {
  const std::int64_t delta = pts_delta_to_microseconds(
      source_pts, source_timebase, origin_pts, origin_timebase);
  if (delta < 0) {
    throw std::invalid_argument(
        "source PTS precedes the primary presentation start");
  }
  return delta;
}

std::int64_t pts_delta_to_microseconds(
    std::int64_t source_pts,
    Rational source_timebase,
    std::int64_t origin_pts,
    Rational origin_timebase) {
  const Rational source = normalize(source_timebase);
  const Rational origin = normalize(origin_timebase);
  const __int128 normalized_numerator =
      static_cast<__int128>(source_pts) * source.numerator *
          origin.denominator -
      static_cast<__int128>(origin_pts) * origin.numerator *
          source.denominator;
  const __int128 denominator =
      static_cast<__int128>(source.denominator) * origin.denominator;
  if (normalized_numerator < 0) {
    return -round_half_to_even_wide(-normalized_numerator * 1000000,
                                    denominator);
  }
  return round_half_to_even_wide(normalized_numerator * 1000000, denominator);
}

std::int64_t frame_index_to_microseconds(std::int64_t frame_index,
                                         Rational frame_rate) {
  const Rational rate = normalize(frame_rate);
  if (frame_index < 0) {
    throw std::invalid_argument("frame index must be non-negative");
  }
  if (rate.numerator <= 0 || rate.denominator <= 0) {
    throw std::invalid_argument("frame rate must be positive");
  }

  const __int128 numerator = static_cast<__int128>(frame_index) *
                             static_cast<__int128>(rate.denominator) * 1000000;
  return round_half_to_even_wide(numerator, static_cast<__int128>(rate.numerator));
}

std::string microseconds_to_seconds_string(std::int64_t microseconds) {
  if (microseconds < 0) {
    throw std::invalid_argument("canonical timestamp must be non-negative");
  }

  const std::int64_t seconds = microseconds / 1000000;
  const std::int64_t fraction = microseconds % 1000000;
  std::string fraction_text = std::to_string(fraction + 1000000).substr(1);
  while (fraction_text.size() > 3 && fraction_text.back() == '0') {
    fraction_text.pop_back();
  }
  return std::to_string(seconds) + "." + fraction_text;
}

}  // namespace svp::media
