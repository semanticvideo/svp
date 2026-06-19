#include "svp/models/manifest.hpp"

#include "svp/models/error.hpp"
#include "svp/models/model_id.hpp"
#include "json_util.hpp"

#include <set>
#include <string_view>
#include <utility>

namespace svp::models {
namespace {

constexpr std::string_view kSchemaVersion = "svp-model-bundle-1";

const std::set<std::string_view> kRuntimeValues = {
    "onnxruntime",
    "whisper.cpp",
    "native",
};

const std::set<std::string_view> kFormatValues = {
    "onnx",
    "ort",
    "gguf",
    "native",
};

const std::set<std::string_view> kExecutionProviderValues = {
    "cpu",
    "coreml",
    "cuda",
    "directml",
    "winml",
};

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

void require_string_value(const std::set<std::string_view>& allowed,
                          std::string_view value,
                          std::string_view property,
                          std::string_view source_name) {
  if (!allowed.contains(value)) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + "." + std::string(property) +
                         " has unsupported value `" + std::string(value) + "`");
  }
}

std::vector<std::string> parse_execution_providers(const nlohmann::json& value,
                                                   std::string_view source_name) {
  const auto& providers =
      detail::require_property(value, "supported_execution_providers", source_name);
  if (!providers.is_array() || providers.empty()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) +
                         ".supported_execution_providers must be a non-empty array");
  }

  std::vector<std::string> result;
  for (std::size_t index = 0; index < providers.size(); ++index) {
    if (!providers[index].is_string()) {
      throw ModelError(ModelErrorCode::schema_error,
                       std::string(source_name) +
                           ".supported_execution_providers entries must be strings");
    }
    std::string provider = providers[index].get<std::string>();
    require_string_value(kExecutionProviderValues, provider,
                         "supported_execution_providers", source_name);
    result.push_back(std::move(provider));
  }
  return result;
}

std::vector<ModelBundleFile> parse_files(const nlohmann::json& value,
                                         std::string_view source_name) {
  const auto& files = detail::require_property(value, "files", source_name);
  if (!files.is_array() || files.empty()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + ".files must be a non-empty array");
  }

  std::vector<ModelBundleFile> result;
  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto& file = files[index];
    const std::string item_source =
        std::string(source_name) + ".files[" + std::to_string(index) + "]";
    detail::require_object(file, item_source);
    detail::reject_unknown_properties(file, {"path", "role", "blake3"}, item_source);

    result.push_back(ModelBundleFile{
        detail::require_non_empty_string(file, "path", item_source),
        detail::require_non_empty_string(file, "role", item_source),
        require_blake3_64(file, "blake3", item_source),
    });
  }
  return result;
}

void require_contract_object(const nlohmann::json& value,
                             std::string_view property,
                             std::string_view source_name) {
  const auto& contract = detail::require_property(value, property, source_name);
  if (!contract.is_object()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) + "." + std::string(property) +
                         " must be an object");
  }
}

std::optional<std::string> read_optional_string(const nlohmann::json& value,
                                                std::string_view property,
                                                std::string_view source_name) {
  if (!detail::has_optional_property(value, property)) {
    return std::nullopt;
  }
  return detail::optional_string(value, property, source_name);
}

}  // namespace

ModelBundleManifest parse_model_bundle_manifest(const nlohmann::json& value,
                                                std::string_view source_name) {
  detail::require_object(value, source_name);
  detail::reject_unknown_properties(
      value,
      {"schema_version",
       "model_bundle_id",
       "model_id",
       "model_version",
       "bundle_blake3",
       "display_name",
       "source_registry",
       "source_slug",
       "source_revision",
       "runtime",
       "format",
       "license",
       "supported_execution_providers",
       "files",
       "input_contract",
       "output_contract",
       "preprocessor_contract",
       "postprocessor_contract"},
      source_name);

  std::string schema_version = detail::require_string(value, "schema_version", source_name);
  if (schema_version != kSchemaVersion) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) +
                         ".schema_version must be svp-model-bundle-1");
  }

  std::string model_bundle_id =
      detail::require_string(value, "model_bundle_id", source_name);
  if (!is_canonical_model_bundle_id(model_bundle_id)) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) +
                         ".model_bundle_id must use canonical model_...@...+blake3_ form");
  }

  std::string model_id = detail::require_string(value, "model_id", source_name);
  if (!is_canonical_model_id(model_id)) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) +
                         ".model_id must be a canonical SVP model_... identifier");
  }

  std::string model_version =
      detail::require_non_empty_string(value, "model_version", source_name);
  svp::core::HashString bundle_blake3 =
      require_blake3_64(value, "bundle_blake3", source_name);

  const std::string expected =
      expected_model_bundle_id(model_id, model_version, bundle_blake3);
  if (model_bundle_id != expected) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) +
                         ".model_bundle_id does not match model_id, model_version, "
                         "and bundle_blake3");
  }

  std::optional<std::string> display_name =
      read_optional_string(value, "display_name", source_name);
  std::optional<std::string> source_registry =
      read_optional_string(value, "source_registry", source_name);
  std::optional<std::string> source_slug =
      read_optional_string(value, "source_slug", source_name);
  std::optional<std::string> source_revision =
      read_optional_string(value, "source_revision", source_name);

  std::string runtime = detail::require_string(value, "runtime", source_name);
  require_string_value(kRuntimeValues, runtime, "runtime", source_name);

  std::string format = detail::require_string(value, "format", source_name);
  require_string_value(kFormatValues, format, "format", source_name);

  std::string license = detail::require_non_empty_string(value, "license", source_name);
  std::vector<std::string> supported_execution_providers =
      parse_execution_providers(value, source_name);
  std::vector<ModelBundleFile> files = parse_files(value, source_name);

  require_contract_object(value, "input_contract", source_name);
  require_contract_object(value, "output_contract", source_name);
  require_contract_object(value, "preprocessor_contract", source_name);
  require_contract_object(value, "postprocessor_contract", source_name);

  return ModelBundleManifest{
      std::move(schema_version),
      std::move(model_bundle_id),
      std::move(model_id),
      std::move(model_version),
      std::move(bundle_blake3),
      std::move(display_name),
      std::move(source_registry),
      std::move(source_slug),
      std::move(source_revision),
      std::move(runtime),
      std::move(format),
      std::move(license),
      std::move(supported_execution_providers),
      std::move(files),
      value.at("input_contract"),
      value.at("output_contract"),
      value.at("preprocessor_contract"),
      value.at("postprocessor_contract"),
  };
}

ModelBundleManifest load_model_bundle_manifest(const std::filesystem::path& path) {
  return parse_model_bundle_manifest(detail::load_json_file(path), path.string());
}

}  // namespace svp::models
