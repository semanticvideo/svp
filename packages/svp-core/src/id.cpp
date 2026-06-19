#include "svp/core/id.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace svp::core {

Identifier::Identifier(std::string value) : value_(std::move(value)) {}

const std::string& Identifier::value() const noexcept {
  return value_;
}

bool Identifier::empty() const noexcept {
  return value_.empty();
}

bool is_portable_identifier(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }

  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '-' || character == '_';
  });
}

}  // namespace svp::core
