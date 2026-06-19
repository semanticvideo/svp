#include "ocr_color_spec.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace svp::validation {
namespace {

nlohmann::json load_json_file(const std::filesystem::path& path) {
  std::ifstream input{path};
  if (!input) {
    throw std::runtime_error("could not open JSON asset: " + path.string());
  }

  nlohmann::json json;
  input >> json;
  return json;
}

std::set<std::string> load_id_set(const nlohmann::json& registry,
                                  const std::string& array_name,
                                  const std::string& id_name) {
  std::set<std::string> ids;
  for (const auto& item : registry.at(array_name)) {
    ids.insert(item.at(id_name).get<std::string>());
  }
  return ids;
}

std::set<std::string> load_string_enum_set(const nlohmann::json& schema,
                                           const std::string& property_name) {
  std::set<std::string> values;
  const auto& enumeration = schema.at("properties").at(property_name).at("enum");
  for (const auto& item : enumeration) {
    values.insert(item.get<std::string>());
  }
  return values;
}

}  // namespace

OcrColorSpec load_ocr_color_spec(const std::filesystem::path& registry_root,
                                 const std::filesystem::path& schema_root) {
  const auto color_buckets = load_json_file(registry_root / "color-buckets.json");
  const auto color_spaces = load_json_file(registry_root / "color-spaces.json");
  const auto ocr_types = load_json_file(registry_root / "ocr-observation-types.json");

  OcrColorSpec spec;
  spec.text_region_schema = load_json_file(schema_root / "text-region.schema.json");
  spec.text_observation_schema = load_json_file(schema_root / "text-observation.schema.json");
  spec.numeric_value_schema = load_json_file(schema_root / "numeric-value.schema.json");
  spec.color_observation_schema = load_json_file(schema_root / "color-observation.schema.json");
  spec.ocr_observation_types = load_id_set(ocr_types, "observation_types", "id");
  spec.color_bucket_ids = load_id_set(color_buckets, "buckets", "bucket_id");
  spec.color_space_ids = load_id_set(color_spaces, "color_spaces", "color_space_id");
  spec.color_sampling_basis_ids =
      load_string_enum_set(spec.color_observation_schema, "sampling_basis");
  spec.color_bucket_registry_version =
      color_buckets.at("registry_version").get<std::string>();
  spec.percentage_sum_tolerance =
      color_buckets.at("percentage_sum_tolerance").get<double>();
  return spec;
}

}  // namespace svp::validation
