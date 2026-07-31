#include "canonical_control_json.hpp"

#include "svp/models/error.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace svp::models::detail {
namespace {

constexpr std::int64_t kDecimalScaleLimit = 1'000'000;

std::vector<std::string> scan_number_tokens(std::string_view input) {
  std::vector<std::string> tokens;
  std::size_t offset = 0;
  while (offset < input.size()) {
    if (input[offset] == '"') {
      ++offset;
      while (offset < input.size()) {
        if (input[offset] == '\\') {
          offset += std::min<std::size_t>(2, input.size() - offset);
        } else if (input[offset++] == '"') {
          break;
        }
      }
      continue;
    }
    if (input[offset] != '-' &&
        (input[offset] < '0' || input[offset] > '9')) {
      ++offset;
      continue;
    }
    const std::size_t begin = offset;
    if (input[offset] == '-') {
      ++offset;
    }
    while (offset < input.size() && input[offset] >= '0' &&
           input[offset] <= '9') {
      ++offset;
    }
    if (offset < input.size() && input[offset] == '.') {
      ++offset;
      while (offset < input.size() && input[offset] >= '0' &&
             input[offset] <= '9') {
        ++offset;
      }
    }
    if (offset < input.size() &&
        (input[offset] == 'e' || input[offset] == 'E')) {
      ++offset;
      if (offset < input.size() &&
          (input[offset] == '+' || input[offset] == '-')) {
        ++offset;
      }
      while (offset < input.size() && input[offset] >= '0' &&
             input[offset] <= '9') {
        ++offset;
      }
    }
    tokens.emplace_back(input.substr(begin, offset - begin));
  }
  return tokens;
}

std::int64_t parse_saturated_exponent(std::string_view digits,
                                      bool negative) {
  std::int64_t value = 0;
  for (const char digit : digits) {
    if (value > (kDecimalScaleLimit - (digit - '0')) / 10) {
      return negative ? -kDecimalScaleLimit : kDecimalScaleLimit;
    }
    value = value * 10 + (digit - '0');
  }
  return negative ? -value : value;
}

struct DecimalNumber {
  bool negative = false;
  std::string coefficient;
  std::int64_t scale = 0;
};

DecimalNumber parse_decimal_number(std::string_view token) {
  DecimalNumber number;
  std::size_t offset = 0;
  if (token[offset] == '-') {
    number.negative = true;
    ++offset;
  }
  const std::size_t exponent_marker = token.find_first_of("eE", offset);
  const std::size_t significand_end =
      exponent_marker == std::string_view::npos ? token.size() : exponent_marker;
  const std::size_t decimal_point = token.find('.', offset);
  const std::size_t integer_end =
      decimal_point == std::string_view::npos || decimal_point >= significand_end
          ? significand_end
          : decimal_point;
  number.coefficient.append(token.substr(offset, integer_end - offset));
  std::size_t fraction_digits = 0;
  if (integer_end < significand_end) {
    fraction_digits = significand_end - integer_end - 1;
    number.coefficient.append(token.substr(integer_end + 1, fraction_digits));
  }
  const std::size_t first_nonzero = number.coefficient.find_first_not_of('0');
  if (first_nonzero == std::string::npos) {
    number.coefficient = "0";
    number.negative = false;
    number.scale = 0;
    return number;
  }
  number.coefficient.erase(0, first_nonzero);

  std::int64_t exponent = 0;
  if (exponent_marker != std::string_view::npos) {
    std::size_t exponent_offset = exponent_marker + 1;
    bool exponent_negative = false;
    if (token[exponent_offset] == '+' || token[exponent_offset] == '-') {
      exponent_negative = token[exponent_offset] == '-';
      ++exponent_offset;
    }
    exponent = parse_saturated_exponent(token.substr(exponent_offset),
                                        exponent_negative);
  }
  const std::int64_t bounded_fraction =
      fraction_digits > static_cast<std::size_t>(kDecimalScaleLimit)
          ? kDecimalScaleLimit
          : static_cast<std::int64_t>(fraction_digits);
  if (exponent <= -kDecimalScaleLimit) {
    number.scale = kDecimalScaleLimit;
  } else if (exponent >= kDecimalScaleLimit) {
    number.scale = -kDecimalScaleLimit;
  } else {
    number.scale = bounded_fraction - exponent;
  }
  return number;
}

bool magnitude_fits(std::string_view magnitude, std::string_view maximum) {
  return magnitude.size() < maximum.size() ||
         (magnitude.size() == maximum.size() && magnitude <= maximum);
}

nlohmann::ordered_json canonical_number(std::string_view token,
                                        const std::filesystem::path& path) {
  DecimalNumber decimal = parse_decimal_number(token);
  if (decimal.coefficient == "0") {
    return std::uint64_t{0};
  }

  std::string integral_magnitude;
  bool is_integral = false;
  if (decimal.scale <= 0) {
    const std::uint64_t zeros = static_cast<std::uint64_t>(-decimal.scale);
    if (zeros <= 20 && decimal.coefficient.size() + zeros <= 20) {
      integral_magnitude = decimal.coefficient + std::string(zeros, '0');
    } else {
      integral_magnitude = decimal.coefficient;
      integral_magnitude.append(21, '0');
    }
    is_integral = true;
  } else if (decimal.scale <=
             static_cast<std::int64_t>(decimal.coefficient.size())) {
    const std::size_t scale = static_cast<std::size_t>(decimal.scale);
    const std::size_t trailing_zeroes =
        decimal.coefficient.size() -
        (decimal.coefficient.find_last_not_of('0') + 1);
    if (trailing_zeroes >= scale) {
      integral_magnitude =
          decimal.coefficient.substr(0, decimal.coefficient.size() - scale);
      is_integral = true;
    }
  }

  if (is_integral) {
    constexpr std::string_view kMaximumUnsigned = "18446744073709551615";
    constexpr std::string_view kMaximumNegativeMagnitude = "9223372036854775808";
    const std::string_view maximum =
        decimal.negative ? kMaximumNegativeMagnitude : kMaximumUnsigned;
    if (!magnitude_fits(integral_magnitude, maximum)) {
      throw ModelError(ModelErrorCode::schema_error,
                       "integral JSON number is outside the canonical CBOR range in " +
                           path.string() + ": " + std::string(token));
    }
    std::uint64_t magnitude = 0;
    const auto result = std::from_chars(integral_magnitude.data(),
                                        integral_magnitude.data() +
                                            integral_magnitude.size(),
                                        magnitude);
    if (result.ec != std::errc{}) {
      throw ModelError(ModelErrorCode::schema_error,
                       "could not canonicalize JSON integer in " + path.string());
    }
    if (!decimal.negative) {
      return magnitude;
    }
    if (magnitude == std::uint64_t{1} << 63U) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
  }

  double value = 0.0;
  try {
    value = nlohmann::ordered_json::parse(token.begin(), token.end())
                .get<double>();
  } catch (const nlohmann::json::exception&) {
    throw ModelError(ModelErrorCode::schema_error,
                     "could not canonicalize JSON number in " + path.string());
  }
  if (!std::isfinite(value) || value == 0.0) {
    throw ModelError(ModelErrorCode::schema_error,
                     "non-integral JSON number is outside the finite binary64 "
                     "range in " + path.string() + ": " + std::string(token));
  }
  return value;
}

void canonicalize_numbers(nlohmann::ordered_json& value,
                          const std::vector<std::string>& tokens,
                          std::size_t& token_index,
                          const std::filesystem::path& path) {
  if (value.is_number()) {
    if (token_index >= tokens.size()) {
      throw ModelError(ModelErrorCode::schema_error,
                       "could not map JSON number tokens in " + path.string());
    }
    value = canonical_number(tokens[token_index++], path);
    return;
  }
  if (value.is_array()) {
    for (nlohmann::ordered_json& item : value) {
      canonicalize_numbers(item, tokens, token_index, path);
    }
  } else if (value.is_object()) {
    for (auto& item : value.items()) {
      canonicalize_numbers(item.value(), tokens, token_index, path);
    }
  }
}

}  // namespace

nlohmann::json parse_canonical_control_json(
    const std::vector<std::uint8_t>& bytes,
    const std::filesystem::path& path) {
  const std::string input(bytes.begin(), bytes.end());
  const std::vector<std::string> number_tokens = scan_number_tokens(input);
  bool duplicate_name = false;
  std::vector<std::set<std::string>> object_names;
  const nlohmann::ordered_json::parser_callback_t callback =
      [&](int, nlohmann::ordered_json::parse_event_t event,
          nlohmann::ordered_json& parsed) {
        if (event == nlohmann::ordered_json::parse_event_t::object_start) {
          object_names.emplace_back();
        } else if (event == nlohmann::ordered_json::parse_event_t::key) {
          if (object_names.empty() ||
              !object_names.back().insert(parsed.get<std::string>()).second) {
            duplicate_name = true;
          }
        } else if (event == nlohmann::ordered_json::parse_event_t::object_end) {
          if (!object_names.empty()) {
            object_names.pop_back();
          }
        }
        return true;
      };

  try {
    nlohmann::ordered_json value = nlohmann::ordered_json::parse(
        input.begin(), input.end(), callback);
    if (duplicate_name) {
      throw ModelError(ModelErrorCode::schema_error,
                       "model bundle control file contains a duplicate object "
                       "name: " + path.string());
    }
    std::size_t token_index = 0;
    canonicalize_numbers(value, number_tokens, token_index, path);
    if (token_index != number_tokens.size()) {
      throw ModelError(ModelErrorCode::schema_error,
                       "could not map JSON number tokens in " + path.string());
    }
    return nlohmann::json(value);
  } catch (const ModelError&) {
    throw;
  } catch (const nlohmann::json::exception& error) {
    throw ModelError(ModelErrorCode::schema_error,
                     "could not parse model bundle control file " + path.string() +
                         ": " + error.what());
  }
}

}  // namespace svp::models::detail
