#include "canonical_cbor.hpp"

#include "svp/models/error.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace svp::models::detail {
namespace {

bool is_valid_utf8(std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first <= 0x7fU) {
      code_point = first;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      continuation_count = 1;
      code_point = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation_count = 2;
      code_point = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation_count = 3;
      code_point = first & 0x07U;
    } else {
      return false;
    }
    if (offset + continuation_count >= value.size()) {
      return false;
    }
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto continuation =
          static_cast<unsigned char>(value[offset + index]);
      if ((continuation & 0xc0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3fU);
    }
    if ((continuation_count == 2 && code_point < 0x800U) ||
        (continuation_count == 3 && code_point < 0x10000U) ||
        (code_point >= 0xd800U && code_point <= 0xdfffU) ||
        code_point > 0x10ffffU) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

bool unsigned_byte_less(std::string_view left, std::string_view right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](char left_byte, char right_byte) {
        return static_cast<unsigned char>(left_byte) <
               static_cast<unsigned char>(right_byte);
      });
}

void append_big_endian(std::vector<std::uint8_t>& output,
                       std::uint64_t value,
                       std::size_t byte_count) {
  for (std::size_t index = 0; index < byte_count; ++index) {
    const std::size_t shift = (byte_count - index - 1) * 8;
    output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void append_cbor_argument(std::vector<std::uint8_t>& output,
                          std::uint8_t major_type,
                          std::uint64_t value) {
  const std::uint8_t major = static_cast<std::uint8_t>(major_type << 5U);
  if (value <= 23U) {
    output.push_back(static_cast<std::uint8_t>(major | value));
  } else if (value <= std::numeric_limits<std::uint8_t>::max()) {
    output.push_back(static_cast<std::uint8_t>(major | 24U));
    append_big_endian(output, value, 1);
  } else if (value <= std::numeric_limits<std::uint16_t>::max()) {
    output.push_back(static_cast<std::uint8_t>(major | 25U));
    append_big_endian(output, value, 2);
  } else if (value <= std::numeric_limits<std::uint32_t>::max()) {
    output.push_back(static_cast<std::uint8_t>(major | 26U));
    append_big_endian(output, value, 4);
  } else {
    output.push_back(static_cast<std::uint8_t>(major | 27U));
    append_big_endian(output, value, 8);
  }
}

void append_canonical_cbor(std::vector<std::uint8_t>& output,
                           const nlohmann::json& value);

void append_cbor_string(std::vector<std::uint8_t>& output,
                        std::string_view value) {
  if (!is_valid_utf8(value)) {
    throw ModelError(ModelErrorCode::schema_error,
                     "model bundle control metadata contains invalid UTF-8");
  }
  append_cbor_argument(output, 3, static_cast<std::uint64_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}

void append_canonical_cbor(std::vector<std::uint8_t>& output,
                           const nlohmann::json& value) {
  if (value.is_null()) {
    output.push_back(0xf6U);
  } else if (value.is_boolean()) {
    output.push_back(value.get<bool>() ? 0xf5U : 0xf4U);
  } else if (value.is_number_unsigned()) {
    append_cbor_argument(output, 0, value.get<std::uint64_t>());
  } else if (value.is_number_integer()) {
    const std::int64_t number = value.get<std::int64_t>();
    if (number >= 0) {
      append_cbor_argument(output, 0, static_cast<std::uint64_t>(number));
    } else {
      append_cbor_argument(output, 1,
                           static_cast<std::uint64_t>(-(number + 1)));
    }
  } else if (value.is_number_float()) {
    output.push_back(0xfbU);
    append_big_endian(output,
                      std::bit_cast<std::uint64_t>(value.get<double>()), 8);
  } else if (value.is_string()) {
    append_cbor_string(output, value.get_ref<const std::string&>());
  } else if (value.is_array()) {
    append_cbor_argument(output, 4, static_cast<std::uint64_t>(value.size()));
    for (const nlohmann::json& item : value) {
      append_canonical_cbor(output, item);
    }
  } else if (value.is_object()) {
    using Member = std::pair<std::string_view, const nlohmann::json*>;
    std::vector<Member> members;
    members.reserve(value.size());
    for (auto iterator = value.cbegin(); iterator != value.cend(); ++iterator) {
      members.emplace_back(iterator.key(), &iterator.value());
    }
    std::sort(members.begin(), members.end(), [](const Member& left,
                                                 const Member& right) {
      return unsigned_byte_less(left.first, right.first);
    });
    append_cbor_argument(output, 5, static_cast<std::uint64_t>(members.size()));
    for (const Member& member : members) {
      append_cbor_string(output, member.first);
      append_canonical_cbor(output, *member.second);
    }
  } else {
    throw ModelError(ModelErrorCode::schema_error,
                     "model bundle control metadata contains an unsupported value");
  }
}

}  // namespace

std::vector<std::uint8_t> encode_canonical_control_cbor(
    const nlohmann::json& value) {
  std::vector<std::uint8_t> bytes;
  append_canonical_cbor(bytes, value);
  return bytes;
}

}  // namespace svp::models::detail
