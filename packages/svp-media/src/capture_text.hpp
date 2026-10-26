#pragma once

// Text values as cameras write them, which may be padded with spaces or NULs.

#include <optional>
#include <string>
#include <string_view>

namespace svp::media {

// The value without leading or trailing whitespace or NULs; none when nothing
// is left.
[[nodiscard]] inline std::optional<std::string> trimmed_text(std::string_view text) {
  constexpr std::string_view kPadding{" \t\r\n\0", 5};
  const std::size_t first = text.find_first_not_of(kPadding);
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t last = text.find_last_not_of(kPadding);
  return std::string(text.substr(first, last - first + 1));
}

}  // namespace svp::media
