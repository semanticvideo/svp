#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace svp::validation {

struct JsonSchemaIssue {
  std::string path;
  std::string message;
};

[[nodiscard]] std::vector<JsonSchemaIssue> validate_json_schema_subset(
    const nlohmann::json& value,
    const nlohmann::json& schema);

}  // namespace svp::validation
