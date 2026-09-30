#include "json_fields.hpp"

#include "svp/exec/exec_error.hpp"

#include <algorithm>

namespace svp::exec::detail {

std::string child_path(std::string_view path, std::string_view name) {
  std::string result(path);
  result.push_back('.');
  result += name;
  return result;
}

void require_object(const nlohmann::json& value, std::string_view path) {
  if (!value.is_object()) {
    throw ExecError(ExecErrorCode::wrong_type,
                    std::string(path) + " must be a JSON object");
  }
}

void reject_unknown_fields(const nlohmann::json& object,
                           std::initializer_list<std::string_view> allowed,
                           std::string_view path) {
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (std::find(allowed.begin(), allowed.end(), iterator.key()) ==
        allowed.end()) {
      throw ExecError(ExecErrorCode::unknown_field,
                      child_path(path, iterator.key()) + " is not a known field");
    }
  }
}

const nlohmann::json& required_field(const nlohmann::json& object,
                                     std::string_view name,
                                     std::string_view path) {
  const auto iterator = object.find(name);
  if (iterator == object.end()) {
    throw ExecError(ExecErrorCode::missing_field,
                    child_path(path, name) + " is required");
  }
  return *iterator;
}

std::string required_string(const nlohmann::json& object, std::string_view name,
                            std::string_view path) {
  const nlohmann::json& value = required_field(object, name, path);
  if (!value.is_string()) {
    throw ExecError(ExecErrorCode::wrong_type,
                    child_path(path, name) + " must be a string");
  }
  return value.get<std::string>();
}

std::uint64_t required_unsigned(const nlohmann::json& object,
                                std::string_view name, std::string_view path) {
  const nlohmann::json& value = required_field(object, name, path);
  // Parsed JSON stores non-negative integers as unsigned; values built in
  // process from `int` literals are signed, so accept those when >= 0.
  const bool non_negative_integer =
      value.is_number_unsigned() ||
      (value.is_number_integer() && value.get<std::int64_t>() >= 0);
  if (!non_negative_integer) {
    throw ExecError(ExecErrorCode::wrong_type,
                    child_path(path, name) + " must be a non-negative integer");
  }
  return value.get<std::uint64_t>();
}

bool required_bool(const nlohmann::json& object, std::string_view name,
                   std::string_view path) {
  const nlohmann::json& value = required_field(object, name, path);
  if (!value.is_boolean()) {
    throw ExecError(ExecErrorCode::wrong_type,
                    child_path(path, name) + " must be a boolean");
  }
  return value.get<bool>();
}

const nlohmann::json& required_object(const nlohmann::json& object,
                                      std::string_view name,
                                      std::string_view path) {
  const nlohmann::json& value = required_field(object, name, path);
  require_object(value, child_path(path, name));
  return value;
}

const nlohmann::json& required_array(const nlohmann::json& object,
                                     std::string_view name,
                                     std::string_view path) {
  const nlohmann::json& value = required_field(object, name, path);
  if (!value.is_array()) {
    throw ExecError(ExecErrorCode::wrong_type,
                    child_path(path, name) + " must be a JSON array");
  }
  return value;
}

Blake3Digest required_blake3_hex(const nlohmann::json& object,
                                 std::string_view name, std::string_view path) {
  const std::string text = required_string(object, name, path);
  const auto digest = parse_blake3_hex(text);
  if (!digest) {
    throw ExecError(ExecErrorCode::invalid_digest,
                    child_path(path, name) +
                        " must be 64 lowercase hexadecimal characters");
  }
  return *digest;
}

Blake3Digest required_blake3_prefixed(const nlohmann::json& object,
                                      std::string_view name,
                                      std::string_view path) {
  const std::string text = required_string(object, name, path);
  const auto digest = parse_blake3_prefixed(text);
  if (!digest) {
    throw ExecError(ExecErrorCode::invalid_digest,
                    child_path(path, name) +
                        " must be \"b3:\" followed by 64 lowercase hexadecimal "
                        "characters");
  }
  return *digest;
}

}  // namespace svp::exec::detail
