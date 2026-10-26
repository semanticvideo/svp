#pragma once

// Capture timestamps as cameras and containers write them, normalized to one
// ISO 8601 form (spec Section 18.3).

#include <optional>
#include <string>
#include <string_view>

namespace svp::media {

// "YYYY-MM-DDTHH:MM:SS[.fff](Z|+HH:MM|+HHMM)" as "YYYY-MM-DDTHH:MM:SS[.fff]"
// plus "Z" or "+HH:MM". A fraction of zeros is dropped. None for anything
// else, including a timestamp with no offset: its zone would be a guess.
[[nodiscard]] std::optional<std::string> normalized_timestamp(std::string_view text);

// EXIF GPSDateStamp "YYYY:MM:DD" and GPSTimeStamp "HH:MM:SS[.fff]", which are
// UTC, as "YYYY-MM-DDTHH:MM:SS[.fff]Z". None when either doesn't read.
[[nodiscard]] std::optional<std::string> exif_gps_utc(std::string_view date,
                                                      std::string_view time);

}  // namespace svp::media
