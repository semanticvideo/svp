#pragma once

#include "svp/models/error.hpp"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>

namespace svp::models::detail {

inline nlohmann::json load_json_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw ModelError(ModelErrorCode::io_error,
                     "could not open JSON file: " + path.string());
  }

  try {
    nlohmann::json value;
    input >> value;
    return value;
  } catch (const nlohmann::json::exception& error) {
    throw ModelError(ModelErrorCode::schema_error,
                     "invalid JSON in " + path.string() + ": " + error.what());
  }
}

inline void require_object(const nlohmann::json& value, std::string_view source_name) {
  if (!value.is_object()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + " must be a JSON object");
  }
}

inline void reject_unknown_properties(
    const nlohmann::json& value,
    std::initializer_list<std::string_view> allowed,
    std::string_view source_name) {
  std::set<std::string_view> allowed_set(allowed.begin(), allowed.end());
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    if (!allowed_set.contains(iterator.key())) {
      throw ModelError(ModelErrorCode::schema_error,
                       std::string(source_name) + " has unknown property `" +
                           iterator.key() + "`");
    }
  }
}

inline const nlohmann::json& require_property(const nlohmann::json& value,
                                              std::string_view property,
                                              std::string_view source_name) {
  const auto iterator = value.find(property);
  if (iterator == value.end()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + " is missing required property `" +
                         std::string(property) + "`");
  }
  return *iterator;
}

inline std::string require_string(const nlohmann::json& value,
                                  std::string_view property,
                                  std::string_view source_name) {
  const auto& property_value = require_property(value, property, source_name);
  if (!property_value.is_string()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + "." + std::string(property) +
                         " must be a string");
  }
  return property_value.get<std::string>();
}

inline std::string require_non_empty_string(const nlohmann::json& value,
                                            std::string_view property,
                                            std::string_view source_name) {
  std::string result = require_string(value, property, source_name);
  if (result.empty()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + "." + std::string(property) +
                         " must not be empty");
  }
  return result;
}

inline bool has_optional_property(const nlohmann::json& value,
                                  std::string_view property) {
  return value.find(property) != value.end();
}

inline std::string optional_string(const nlohmann::json& value,
                                   std::string_view property,
                                   std::string_view source_name) {
  const auto iterator = value.find(property);
  if (iterator == value.end()) {
    return {};
  }
  if (!iterator->is_string()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + "." + std::string(property) +
                         " must be a string");
  }
  return iterator->get<std::string>();
}

}  // namespace svp::models::detail
