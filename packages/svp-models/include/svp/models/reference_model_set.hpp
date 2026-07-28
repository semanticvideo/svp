#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::models {

struct ReferenceModelFile {
  std::string path;
  std::string role;
  std::string blake3;
};

struct ReferenceModel {
  std::string model_id;
  std::optional<std::string> display_name;
  std::optional<std::string> source_slug;
  std::optional<std::string> source_revision;
  std::optional<std::string> license;
  std::optional<std::string> model_bundle_id;
  std::optional<std::string> bundle_blake3;
  std::vector<ReferenceModelFile> required_files;
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
