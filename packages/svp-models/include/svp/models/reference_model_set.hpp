#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::models {

struct ReferenceModel {
  std::string model_id;
  std::optional<std::string> display_name;
  std::optional<std::string> source_slug;
  std::vector<std::string> required_for;
};

struct ReferenceModelSet {
  std::string schema_version;
  std::optional<std::string> svp_version;
  std::optional<std::string> model_id_policy;
  std::vector<ReferenceModel> models;
};

[[nodiscard]] ReferenceModelSet parse_reference_model_set(
    const nlohmann::json& value,
    std::string_view source_name);
[[nodiscard]] ReferenceModelSet load_reference_model_set(
    const std::filesystem::path& path);

}  // namespace svp::models
