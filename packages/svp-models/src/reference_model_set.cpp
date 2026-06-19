#include "svp/models/reference_model_set.hpp"

#include "svp/models/error.hpp"
#include "svp/models/model_id.hpp"
#include "json_util.hpp"

#include <string_view>
#include <utility>

namespace svp::models {
namespace {

constexpr std::string_view kSchemaVersion = "svp-reference-model-set-1";

std::optional<std::string> read_optional_string(const nlohmann::json& value,
                                                std::string_view property,
                                                std::string_view source_name) {
  if (!detail::has_optional_property(value, property)) {
    return std::nullopt;
  }
  return detail::optional_string(value, property, source_name);
}

std::vector<std::string> parse_required_for(const nlohmann::json& value,
                                            std::string_view source_name) {
  const auto& required_for = detail::require_property(value, "required_for", source_name);
  if (!required_for.is_array()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + ".required_for must be an array");
  }

  std::vector<std::string> result;
  for (std::size_t index = 0; index < required_for.size(); ++index) {
    if (!required_for[index].is_string()) {
      throw ModelError(ModelErrorCode::schema_error,
                       std::string(source_name) +
                           ".required_for entries must be strings");
    }
    result.push_back(required_for[index].get<std::string>());
  }
  return result;
}

}  // namespace

ReferenceModelSet parse_reference_model_set(const nlohmann::json& value,
                                            std::string_view source_name) {
  detail::require_object(value, source_name);
  detail::reject_unknown_properties(
      value, {"schema_version", "svp_version", "model_id_policy", "models"},
      source_name);

  ReferenceModelSet model_set;
  model_set.schema_version = detail::require_string(value, "schema_version", source_name);
  if (model_set.schema_version != kSchemaVersion) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) +
                         ".schema_version must be svp-reference-model-set-1");
  }

  model_set.svp_version = read_optional_string(value, "svp_version", source_name);
  model_set.model_id_policy =
      read_optional_string(value, "model_id_policy", source_name);

  const auto& models = detail::require_property(value, "models", source_name);
  if (!models.is_array() || models.empty()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + ".models must be a non-empty array");
  }

  for (std::size_t index = 0; index < models.size(); ++index) {
    const auto& model = models[index];
    const std::string item_source =
        std::string(source_name) + ".models[" + std::to_string(index) + "]";
    detail::require_object(model, item_source);
    detail::reject_unknown_properties(
        model, {"model_id", "display_name", "source_slug", "required_for"},
        item_source);

    ReferenceModel reference;
    reference.model_id = detail::require_string(model, "model_id", item_source);
    if (!is_canonical_model_id(reference.model_id)) {
      throw ModelError(ModelErrorCode::schema_error,
                       item_source + ".model_id must be a canonical SVP model_... "
                                     "identifier");
    }
    reference.display_name = read_optional_string(model, "display_name", item_source);
    reference.source_slug = read_optional_string(model, "source_slug", item_source);
    reference.required_for = parse_required_for(model, item_source);
    model_set.models.push_back(std::move(reference));
  }

  return model_set;
}

ReferenceModelSet load_reference_model_set(const std::filesystem::path& path) {
  return parse_reference_model_set(detail::load_json_file(path), path.string());
}

}  // namespace svp::models
