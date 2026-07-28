#include "svp/models/model_lock.hpp"

#include "svp/models/error.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/model_id.hpp"
#include "json_util.hpp"

#include <set>
#include <string_view>
#include <utility>

namespace svp::models {
namespace {

constexpr std::string_view kSchemaVersion = "svp-model-lock-1";

svp::core::HashString require_blake3_64(const nlohmann::json& value,
                                        std::string_view property,
                                        std::string_view source_name) {
  std::string raw = detail::require_string(value, property, source_name);
  std::optional<svp::core::HashString> parsed = svp::core::parse_hash_string(raw);
  if (!parsed.has_value() || parsed->hex_value().size() != 64) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + "." + std::string(property) +
                         " must be a blake3: hash with 64 lowercase hex characters");
  }
  return std::move(*parsed);
}

}  // namespace

ModelLock parse_model_lock(const nlohmann::json& value, std::string_view source_name) {
  detail::require_object(value, source_name);
  detail::reject_unknown_properties(value, {"schema_version", "model_set_id", "models"},
                                    source_name);

  ModelLock lock;
  lock.schema_version = detail::require_string(value, "schema_version", source_name);
  if (lock.schema_version != kSchemaVersion) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) +
                         ".schema_version must be svp-model-lock-1");
  }

  lock.model_set_id = detail::require_non_empty_string(value, "model_set_id", source_name);

  const auto& models = detail::require_property(value, "models", source_name);
  if (!models.is_array() || models.empty()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + ".models must be a non-empty array");
  }

  std::set<std::string> model_ids;
  std::set<std::string> model_bundle_ids;
  for (std::size_t index = 0; index < models.size(); ++index) {
    const auto& model = models[index];
    const std::string item_source =
        std::string(source_name) + ".models[" + std::to_string(index) + "]";
    detail::require_object(model, item_source);
    detail::reject_unknown_properties(
        model,
        {"model_id", "model_bundle_id", "model_version", "bundle_blake3", "files"},
        item_source);

    std::string model_id = detail::require_string(model, "model_id", item_source);
    if (!is_canonical_model_id(model_id)) {
      throw ModelError(ModelErrorCode::schema_error,
                       item_source + ".model_id must be a canonical SVP model_... "
                                     "identifier");
    }
    if (!model_ids.insert(model_id).second) {
      throw ModelError(ModelErrorCode::schema_error,
                       item_source + ".model_id duplicates another lock entry");
    }

    std::string model_bundle_id =
        detail::require_string(model, "model_bundle_id", item_source);
    if (!is_canonical_model_bundle_id(model_bundle_id)) {
      throw ModelError(ModelErrorCode::schema_error,
                       item_source +
                           ".model_bundle_id must use canonical model_...@...+blake3_ "
                           "form");
    }
    if (!model_bundle_ids.insert(model_bundle_id).second) {
      throw ModelError(ModelErrorCode::schema_error,
                       item_source +
                           ".model_bundle_id duplicates another lock entry");
    }

    std::string model_version =
        detail::require_non_empty_string(model, "model_version", item_source);
    svp::core::HashString bundle_blake3 =
        require_blake3_64(model, "bundle_blake3", item_source);

    const std::string expected =
        expected_model_bundle_id(model_id, model_version, bundle_blake3);
    if (model_bundle_id != expected) {
      throw ModelError(ModelErrorCode::schema_error,
                       item_source +
                           ".model_bundle_id does not match model_id, model_version, "
                           "and bundle_blake3");
    }

    const auto& files = detail::require_property(model, "files", item_source);
    if (!files.is_array() || files.empty()) {
      throw ModelError(ModelErrorCode::schema_error,
                       item_source + ".files must be a non-empty array");
    }
    std::vector<ModelBundleFile> locked_files;
    std::set<std::string> file_paths;
    for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
      const auto& file = files[file_index];
      const std::string file_source =
          item_source + ".files[" + std::to_string(file_index) + "]";
      detail::require_object(file, file_source);
      detail::reject_unknown_properties(file, {"path", "role", "blake3"},
                                        file_source);
      std::string path =
          detail::require_non_empty_string(file, "path", file_source);
      if (!file_paths.insert(path).second) {
        throw ModelError(ModelErrorCode::schema_error,
                         file_source + ".path duplicates another file entry");
      }
      std::string role =
          detail::require_non_empty_string(file, "role", file_source);
      locked_files.push_back(
          ModelBundleFile{std::move(path), std::move(role),
                          require_blake3_64(file, "blake3", file_source)});
    }

    lock.models.push_back(ModelLockEntry{
        std::move(model_id), std::move(model_bundle_id), std::move(model_version),
        std::move(bundle_blake3), std::move(locked_files)});
  }

  return lock;
}

ModelLock load_model_lock(const std::filesystem::path& path) {
  return parse_model_lock(detail::load_json_file(path), path.string());
}

}  // namespace svp::models
