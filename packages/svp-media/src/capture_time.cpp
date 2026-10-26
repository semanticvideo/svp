#include "capture_time.hpp"

namespace svp::media {
namespace {

bool digits(std::string_view text, std::size_t at, std::size_t count) {
  if (at + count > text.size()) {
    return false;
  }
  for (std::size_t i = at; i < at + count; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
  }
  return true;
}

// "HH:MM:SS" then an optional ".fff"; returns where it ends, or npos.
std::size_t clock_end(std::string_view text, std::size_t at) {
  if (!digits(text, at, 2) || text.size() < at + 8 || text[at + 2] != ':' ||
      !digits(text, at + 3, 2) || text[at + 5] != ':' || !digits(text, at + 6, 2)) {
    return std::string_view::npos;
  }
  std::size_t end = at + 8;
  if (end < text.size() && text[end] == '.') {
    std::size_t fraction = end + 1;
    while (fraction < text.size() && text[fraction] >= '0' && text[fraction] <= '9') {
      ++fraction;
    }
    if (fraction == end + 1) {
      return std::string_view::npos;
    }
    end = fraction;
  }
  return end;
}

// The clock as written, with a fraction of only zeros dropped.
std::string trimmed_clock(std::string_view clock) {
  const std::size_t point = clock.find('.');
  if (point == std::string_view::npos) {
    return std::string(clock);
  }
  std::string_view fraction = clock.substr(point + 1);
  while (!fraction.empty() && fraction.back() == '0') {
    fraction.remove_suffix(1);
  }
  std::string out(clock.substr(0, point));
  if (!fraction.empty()) {
    out += '.';
    out += fraction;
  }
  return out;
}

}  // namespace

std::optional<std::string> normalized_timestamp(std::string_view text) {
  // YYYY-MM-DD 'T'
  if (!digits(text, 0, 4) || text.size() < 11 || text[4] != '-' || !digits(text, 5, 2) ||
      text[7] != '-' || !digits(text, 8, 2) || text[10] != 'T') {
    return std::nullopt;
  }
  const std::size_t end = clock_end(text, 11);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  std::string out(text.substr(0, 11));
  out += trimmed_clock(text.substr(11, end - 11));
  const std::string_view zone = text.substr(end);
  if (zone == "Z") {
    return out + "Z";
  }
  if (zone.size() == 6 && (zone[0] == '+' || zone[0] == '-') && digits(zone, 1, 2) &&
      zone[3] == ':' && digits(zone, 4, 2)) {
    return out + std::string(zone);
  }
  if (zone.size() == 5 && (zone[0] == '+' || zone[0] == '-') && digits(zone, 1, 4)) {
    return out + std::string(zone.substr(0, 3)) + ":" + std::string(zone.substr(3));
  }
  return std::nullopt;
}

std::optional<std::string> exif_gps_utc(std::string_view date, std::string_view time) {
  if (date.size() != 10 || !digits(date, 0, 4) || date[4] != ':' || !digits(date, 5, 2) ||
      date[7] != ':' || !digits(date, 8, 2)) {
    return std::nullopt;
  }
  const std::size_t end = clock_end(time, 0);
  if (end != time.size()) {
    return std::nullopt;
  }
  std::string out(date);
  out[4] = '-';
  out[7] = '-';
  return out + "T" + trimmed_clock(time) + "Z";
}

}  // namespace svp::media
