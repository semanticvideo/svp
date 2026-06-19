#include "json_schema_subset.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace svp::validation {
namespace {

std::string child_path(std::string_view parent, std::string_view child) {
  if (parent == "$") {
    return "$." + std::string{child};
  }

  return std::string{parent} + "." + std::string{child};
}

std::string item_path(std::string_view parent, std::size_t index) {
  std::ostringstream output;
  output << parent << "[" << index << "]";
  return output.str();
}

void add_issue(std::vector<JsonSchemaIssue>& issues,
               std::string path,
               std::string message) {
  issues.push_back(JsonSchemaIssue{.path = std::move(path), .message = std::move(message)});
}

bool matches_type(const nlohmann::json& value, const std::string& type) {
  if (type == "object") {
    return value.is_object();
  }
  if (type == "array") {
    return value.is_array();
  }
  if (type == "string") {
    return value.is_string();
  }
  if (type == "integer") {
    return value.is_number_integer();
  }
  if (type == "number") {
    return value.is_number() && std::isfinite(value.get<double>());
  }
  if (type == "boolean") {
    return value.is_boolean();
  }

  return false;
}

std::string type_name(const nlohmann::json& value) {
  return std::string{value.type_name()};
}

void validate_node(const nlohmann::json& value,
                   const nlohmann::json& schema,
                   const std::string& path,
                   std::vector<JsonSchemaIssue>& issues);

void validate_object(const nlohmann::json& value,
                     const nlohmann::json& schema,
                     const std::string& path,
                     std::vector<JsonSchemaIssue>& issues) {
  const auto properties = schema.find("properties");
  if (const auto required = schema.find("required");
      required != schema.end() && required->is_array()) {
    for (const auto& required_name : *required) {
      if (!required_name.is_string()) {
        continue;
      }
      const auto name = required_name.get<std::string>();
      if (!value.contains(name)) {
        add_issue(issues, child_path(path, name), "required property is missing");
      }
    }
  }

  for (const auto& [name, property_value] : value.items()) {
    if (properties != schema.end() && properties->is_object() && properties->contains(name)) {
      validate_node(property_value, properties->at(name), child_path(path, name), issues);
      continue;
    }

    const auto additional = schema.find("additionalProperties");
    if (additional != schema.end()) {
      if (additional->is_boolean() && !additional->get<bool>()) {
        add_issue(issues, child_path(path, name), "additional property is not allowed");
        continue;
      }
      if (additional->is_object()) {
        validate_node(property_value, *additional, child_path(path, name), issues);
      }
    }
  }
}

void validate_array(const nlohmann::json& value,
                    const nlohmann::json& schema,
                    const std::string& path,
                    std::vector<JsonSchemaIssue>& issues) {
  if (const auto min_items = schema.find("minItems");
      min_items != schema.end() && min_items->is_number_unsigned() &&
      value.size() < min_items->get<std::size_t>()) {
    add_issue(issues, path, "array has too few items");
  }

  if (const auto max_items = schema.find("maxItems");
      max_items != schema.end() && max_items->is_number_unsigned() &&
      value.size() > max_items->get<std::size_t>()) {
    add_issue(issues, path, "array has too many items");
  }

  if (const auto items = schema.find("items"); items != schema.end() && items->is_object()) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      validate_node(value.at(index), *items, item_path(path, index), issues);
    }
  }
}

void validate_enum(const nlohmann::json& value,
                   const nlohmann::json& schema,
                   const std::string& path,
                   std::vector<JsonSchemaIssue>& issues) {
  const auto enumeration = schema.find("enum");
  if (enumeration == schema.end() || !enumeration->is_array()) {
    return;
  }

  for (const auto& allowed : *enumeration) {
    if (value == allowed) {
      return;
    }
  }

  add_issue(issues, path, "value is not in the allowed enum");
}

void validate_number_bounds(const nlohmann::json& value,
                            const nlohmann::json& schema,
                            const std::string& path,
                            std::vector<JsonSchemaIssue>& issues) {
  if (!value.is_number()) {
    return;
  }

  const auto numeric_value = value.get<double>();
  if (const auto minimum = schema.find("minimum");
      minimum != schema.end() && minimum->is_number() &&
      numeric_value < minimum->get<double>()) {
    add_issue(issues, path, "number is below minimum");
  }

  if (const auto maximum = schema.find("maximum");
      maximum != schema.end() && maximum->is_number() &&
      numeric_value > maximum->get<double>()) {
    add_issue(issues, path, "number is above maximum");
  }
}

void validate_pattern(const nlohmann::json& value,
                      const nlohmann::json& schema,
                      const std::string& path,
                      std::vector<JsonSchemaIssue>& issues) {
  const auto pattern = schema.find("pattern");
  if (pattern == schema.end() || !pattern->is_string() || !value.is_string()) {
    return;
  }

  try {
    const std::regex expression{pattern->get<std::string>()};
    if (!std::regex_match(value.get<std::string>(), expression)) {
      add_issue(issues, path, "string does not match required pattern");
    }
  } catch (const std::regex_error&) {
    add_issue(issues, path, "schema pattern is not supported by this validator");
  }
}

void validate_node(const nlohmann::json& value,
                   const nlohmann::json& schema,
                   const std::string& path,
                   std::vector<JsonSchemaIssue>& issues) {
  if (const auto type = schema.find("type"); type != schema.end() && type->is_string()) {
    const auto type_string = type->get<std::string>();
    if (!matches_type(value, type_string)) {
      add_issue(issues, path,
                "expected " + type_string + " but found " + type_name(value));
      return;
    }
  }

  validate_enum(value, schema, path, issues);
  validate_number_bounds(value, schema, path, issues);
  validate_pattern(value, schema, path, issues);

  if (value.is_object()) {
    validate_object(value, schema, path, issues);
  } else if (value.is_array()) {
    validate_array(value, schema, path, issues);
  }
}

}  // namespace

std::vector<JsonSchemaIssue> validate_json_schema_subset(const nlohmann::json& value,
                                                         const nlohmann::json& schema) {
  std::vector<JsonSchemaIssue> issues;
  validate_node(value, schema, "$", issues);
  return issues;
}

}  // namespace svp::validation
