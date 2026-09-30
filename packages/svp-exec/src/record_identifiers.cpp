#include "record_identifiers.hpp"

#include <algorithm>
#include <cstddef>

namespace svp::exec::detail {
namespace {

// RFC 6838 §4.2: type and subtype restricted-names are at most 127 characters.
constexpr std::size_t kMaxMediaTypeNameChars = 127;

bool is_lower_alnum(char character) noexcept {
  return (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9');
}

bool is_alnum(char character) noexcept {
  return is_lower_alnum(character) || (character >= 'A' && character <= 'Z');
}

bool is_restricted_name(std::string_view name) noexcept {
  constexpr std::string_view kRestrictedNameSymbols = "!#$&-^_.+";
  if (name.empty() || name.size() > kMaxMediaTypeNameChars ||
      !is_lower_alnum(name.front())) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [&](char character) {
    return is_lower_alnum(character) ||
           kRestrictedNameSymbols.find(character) != std::string_view::npos;
  });
}

}  // namespace

bool is_record_identifier(std::string_view value) noexcept {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return is_alnum(character) || character == '.' || character == '_' ||
                  character == '-';
         });
}

bool is_lower_identifier(std::string_view value) noexcept {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return is_lower_alnum(character) || character == '.' ||
                  character == '_';
         });
}

bool is_media_type(std::string_view value) noexcept {
  const std::size_t slash = value.find('/');
  if (slash == std::string_view::npos) {
    return false;
  }
  return is_restricted_name(value.substr(0, slash)) &&
         is_restricted_name(value.substr(slash + 1));
}

}  // namespace svp::exec::detail
