#pragma once

#include "svp/core/hash_string.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::models {

struct ModelLockEntry {
  std::string model_id;
  std::string model_bundle_id;
  std::string model_version;
  svp::core::HashString bundle_blake3;
};

struct ModelLock {
  std::string schema_version;
  std::string model_set_id;
  std::vector<ModelLockEntry> models;
};

[[nodiscard]] ModelLock parse_model_lock(const nlohmann::json& value,
                                         std::string_view source_name);
[[nodiscard]] ModelLock load_model_lock(const std::filesystem::path& path);

}  // namespace svp::models
