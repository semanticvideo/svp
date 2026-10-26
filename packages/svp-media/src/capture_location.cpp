#include "svp/media/capture_location.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

namespace svp::media {
namespace {

constexpr double kMaxLatitude = 90.0;
constexpr double kMaxLongitude = 180.0;
constexpr double kMinutesPerDegree = 60.0;
constexpr double kSecondsPerDegree = 3600.0;

}  // namespace

std::optional<double> parse_unsigned_decimal(std::string_view text) {
  // Digits with at most one point, nothing else: strtod alone would also take
  // signs, spaces, exponents, hex and "inf". Floating-point from_chars isn't in
  // every supported libc++.
  bool digit = false;
  bool point = false;
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      digit = true;
    } else if (c == '.' && !point) {
      point = true;
    } else {
      return std::nullopt;
    }
  }
  if (!digit) {
    return std::nullopt;
  }
  const std::string copy(text);
  const double value = std::strtod(copy.c_str(), nullptr);
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

namespace {

std::optional<double> parse_number(std::string_view text) {
  return parse_unsigned_decimal(text);
}

// One signed ISO 6709 component starting at `pos`: its digits and point up to
// the next sign or '/'. The integer part's width says the form:
// `degree_digits` (D.D), +2 (DDMM.M) or +4 (DDMMSS.S).
std::optional<double> iso6709_component(std::string_view text, std::size_t& pos,
                                        std::size_t degree_digits, double max_degrees) {
  if (pos >= text.size() || (text[pos] != '+' && text[pos] != '-')) {
    return std::nullopt;
  }
  const double sign = text[pos] == '-' ? -1.0 : 1.0;
  const std::size_t start = ++pos;
  while (pos < text.size() && text[pos] != '+' && text[pos] != '-' && text[pos] != '/') {
    ++pos;
  }
  const std::string_view body = text.substr(start, pos - start);
  const std::size_t point = body.find('.');
  const std::size_t integer_digits = point == std::string_view::npos ? body.size() : point;
  const auto number = parse_number(body);
  if (!number) {
    return std::nullopt;
  }
  double degrees = 0.0;
  if (integer_digits == degree_digits) {
    degrees = *number;
  } else if (integer_digits == degree_digits + 2) {
    const double whole_minutes = std::floor(*number / 100.0);
    degrees = whole_minutes + (*number - whole_minutes * 100.0) / kMinutesPerDegree;
  } else if (integer_digits == degree_digits + 4) {
    const double d = std::floor(*number / 10000.0);
    const double m = std::floor((*number - d * 10000.0) / 100.0);
    const double s = *number - d * 10000.0 - m * 100.0;
    degrees = d + m / kMinutesPerDegree + s / kSecondsPerDegree;
  } else {
    return std::nullopt;
  }
  if (degrees > max_degrees) {
    return std::nullopt;
  }
  return sign * degrees;
}

}  // namespace

std::optional<GeoPoint> parse_iso6709(std::string_view text) {
  // Latitude has two degree digits and longitude three (ISO 6709 Annex H).
  std::size_t pos = 0;
  const auto latitude = iso6709_component(text, pos, 2, kMaxLatitude);
  const auto longitude = iso6709_component(text, pos, 3, kMaxLongitude);
  if (!latitude || !longitude) {
    return std::nullopt;
  }
  GeoPoint point{.latitude = *latitude, .longitude = *longitude};
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    const double sign = text[pos] == '-' ? -1.0 : 1.0;
    const std::size_t start = ++pos;
    while (pos < text.size() && text[pos] != '/' && text[pos] != 'C') {
      ++pos;
    }
    const auto altitude = parse_number(text.substr(start, pos - start));
    if (!altitude) {
      return std::nullopt;
    }
    point.altitude_m = sign * *altitude;
  }
  return point;
}

std::optional<double> parse_exif_dms(std::string_view value, std::string_view reference,
                                     double max_degrees) {
  double sign = 0.0;
  if (reference == "N" || reference == "E") {
    sign = 1.0;
  } else if (reference == "S" || reference == "W") {
    sign = -1.0;
  } else {
    return std::nullopt;
  }
  double parts[3] = {0.0, 0.0, 0.0};
  std::size_t count = 0;
  std::size_t start = 0;
  while (count < 3) {
    const std::size_t end = value.find(';', start);
    const auto part = parse_number(value.substr(start, end == std::string_view::npos
                                                           ? std::string_view::npos
                                                           : end - start));
    if (!part || *part < 0.0) {
      return std::nullopt;
    }
    parts[count++] = *part;
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (count != 3 || parts[1] >= kMinutesPerDegree || parts[2] >= kMinutesPerDegree) {
    return std::nullopt;
  }
  const double degrees =
      parts[0] + parts[1] / kMinutesPerDegree + parts[2] / kSecondsPerDegree;
  if (degrees > max_degrees) {
    return std::nullopt;
  }
  return sign * degrees;
}

}  // namespace svp::media
