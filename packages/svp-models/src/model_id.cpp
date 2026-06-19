#include "svp/models/model_id.hpp"

#include "svp/core/hash_string.hpp"

#include <algorithm>
#include <cctype>

namespace svp::models {
namespace {

bool is_lower_hex_12(std::string_view value) noexcept {
  return value.size() == 12 && svp::core::is_lower_hex(value);
}

}  // namespace

bool is_canonical_model_id(std::string_view value) noexcept {
  constexpr std::string_view prefix = "model_";
  if (!value.starts_with(prefix) || value.size() == prefix.size()) {
    return false;
  }

  return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                     value.end(), [](unsigned char character) {
                       return std::isdigit(character) != 0 ||
                              (character >= static_cast<unsigned char>('a') &&
                               character <= static_cast<unsigned char>('z')) ||
                              character == static_cast<unsigned char>('_');
                     });
}

std::optional<ModelBundleIdParts> parse_model_bundle_id(std::string_view value) {
  constexpr std::string_view hash_marker = "+blake3_";

  const std::size_t at_position = value.find('@');
  const std::size_t marker_position = value.rfind(hash_marker);
  if (at_position == std::string_view::npos ||
      marker_position == std::string_view::npos ||
      marker_position <= at_position + 1) {
    return std::nullopt;
  }

  std::string_view model_id = value.substr(0, at_position);
  std::string_view model_version =
      value.substr(at_position + 1, marker_position - at_position - 1);
  std::string_view hash_prefix =
      value.substr(marker_position + hash_marker.size());

  if (!is_canonical_model_id(model_id) || model_version.empty() ||
      model_version.find('+') != std::string_view::npos ||
      !is_lower_hex_12(hash_prefix)) {
    return std::nullopt;
  }

  return ModelBundleIdParts{std::string(model_id), std::string(model_version),
                            std::string(hash_prefix)};
}

bool is_canonical_model_bundle_id(std::string_view value) {
  return parse_model_bundle_id(value).has_value();
}

std::string expected_model_bundle_id(std::string_view model_id,
                                     std::string_view model_version,
                                     const svp::core::HashString& bundle_blake3) {
  std::string result(model_id);
  result.push_back('@');
  result += model_version;
  result += "+blake3_";
  result += bundle_blake3.hex_value().substr(0, 12);
  return result;
}

}  // namespace svp::models
