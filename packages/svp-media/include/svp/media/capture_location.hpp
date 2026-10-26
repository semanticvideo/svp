#pragma once

// Geographic coordinates as cameras write them, read into WGS-84 decimal
// degrees (spec Section 18.3).

#include <optional>
#include <string_view>

namespace svp::media {

struct GeoPoint {
  double latitude = 0.0;
  double longitude = 0.0;
  std::optional<double> altitude_m;
};

// A plain unsigned decimal, "30.240": digits and at most one point. None for
// anything else, so signs, exponents and "inf" are never taken as numbers.
[[nodiscard]] std::optional<double> parse_unsigned_decimal(std::string_view text);

// ISO 6709 point strings as QuickTime and MP4 `location` tags carry them, e.g.
// "+48.8584+002.2945+035.000/": degrees, degrees-minutes or
// degrees-minutes-seconds forms, with an optional altitude. None when the
// string isn't one, or a coordinate is out of range.
[[nodiscard]] std::optional<GeoPoint> parse_iso6709(std::string_view text);

// An EXIF GPS degrees-minutes-seconds value as Sony's XML writes it,
// "48;51;30.240", signed by its reference ("N"/"S" or "E"/"W"). None when it
// isn't one, or the result is out of range for `max_degrees`.
[[nodiscard]] std::optional<double> parse_exif_dms(std::string_view value,
                                                   std::string_view reference,
                                                   double max_degrees);

}  // namespace svp::media
