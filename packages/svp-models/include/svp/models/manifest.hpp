#pragma once

#include "svp/core/hash_string.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::models {

struct ModelBundleFile {
  std::string path;
  std::string role;
  svp::core::HashString blake3;
};

struct ModelBundleManifest {
  std::string schema_version;
  std::string model_bundle_id;
  std::string model_id;
  std::string model_version;
  svp::core::HashString bundle_blake3;
  std::optional<std::string> display_name;
  std::optional<std::string> source_registry;
  std::optional<std::string> source_slug;
  std::optional<std::string> source_revision;
  std::string runtime;
  std::string format;
  std::string license;
  std::vector<std::string> supported_execution_providers;
  std::vector<ModelBundleFile> files;
  nlohmann::json input_contract;
  nlohmann::json output_contract;
  nlohmann::json preprocessor_contract;
  nlohmann::json postprocessor_contract;
};

[[nodiscard]] ModelBundleManifest parse_model_bundle_manifest(
    const nlohmann::json& value,
    std::string_view source_name);

[[nodiscard]] ModelBundleManifest load_model_bundle_manifest(
    const std::filesystem::path& path);

}  // namespace svp::models
