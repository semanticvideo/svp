#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <set>
#include <string>

namespace svp::validation {

struct OcrColorSpec {
  nlohmann::json text_region_schema;
  nlohmann::json text_observation_schema;
  nlohmann::json numeric_value_schema;
  nlohmann::json color_observation_schema;
  std::set<std::string> ocr_observation_types;
  std::set<std::string> color_bucket_ids;
  std::set<std::string> color_space_ids;
  std::set<std::string> color_sampling_basis_ids;
  std::string color_bucket_registry_version;
  double percentage_sum_tolerance = 0.001;
};

[[nodiscard]] OcrColorSpec load_ocr_color_spec(const std::filesystem::path& registry_root,
                                               const std::filesystem::path& schema_root);

}  // namespace svp::validation
