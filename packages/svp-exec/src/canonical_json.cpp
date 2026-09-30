#include "svp/exec/canonical_json.hpp"

#include "svp/exec/exec_error.hpp"

#include <cmath>
#include <string>

namespace svp::exec {
namespace {

void require_encodable(const nlohmann::json& value, std::size_t depth) {
  switch (value.type()) {
    case nlohmann::json::value_t::object:
    case nlohmann::json::value_t::array:
      if (depth >= kMaxCanonicalJsonDepth) {
        throw ExecError(ExecErrorCode::invalid_value,
                        "JSON nesting exceeds the canonical depth limit");
      }
      for (const auto& child : value) {
        require_encodable(child, depth + 1);
      }
      return;
    case nlohmann::json::value_t::number_float:
      if (!std::isfinite(value.get<double>())) {
        throw ExecError(ExecErrorCode::non_finite_number,
                        "canonical JSON cannot represent NaN or Infinity");
      }
      return;
    case nlohmann::json::value_t::binary:
    case nlohmann::json::value_t::discarded:
      throw ExecError(ExecErrorCode::wrong_type,
                      "canonical JSON cannot represent binary or discarded values");
    default:
      return;
  }
}

// Linear scan run before parsing so a deeply nested document is rejected
// without building (or recursively destroying) its DOM.
void require_bounded_depth(std::string_view bytes) {
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char character : bytes) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
    } else if (character == '{' || character == '[') {
      if (++depth > kMaxCanonicalJsonDepth) {
        throw ExecError(ExecErrorCode::invalid_value,
                        "JSON nesting exceeds the canonical depth limit");
      }
    } else if ((character == '}' || character == ']') && depth > 0) {
      --depth;
    }
  }
}

}  // namespace

std::string encode_canonical_json(const nlohmann::json& value) {
  require_encodable(value, 0);
  try {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
  } catch (const nlohmann::json::type_error& error) {
    throw ExecError(ExecErrorCode::invalid_json,
                    std::string("canonical JSON requires valid UTF-8: ") +
                        error.what());
  }
}

nlohmann::json decode_canonical_json(std::string_view bytes) {
  require_bounded_depth(bytes);
  nlohmann::json value;
  try {
    value = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr,
                                  /*allow_exceptions=*/true,
                                  /*ignore_comments=*/false);
  } catch (const nlohmann::json::exception& error) {
    throw ExecError(ExecErrorCode::invalid_json,
                    std::string("invalid JSON: ") + error.what());
  }
  if (encode_canonical_json(value) != bytes) {
    throw ExecError(ExecErrorCode::non_canonical_json,
                    "JSON bytes are not in canonical form");
  }
  return value;
}

}  // namespace svp::exec
