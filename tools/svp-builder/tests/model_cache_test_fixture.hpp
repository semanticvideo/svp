#pragma once

#include "svp/models/hash.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::test {

inline void write_text(const std::filesystem::path& path,
                       std::string_view value) {
  std::ofstream output(path, std::ios::binary);
  output << value;
}

inline std::string file_digest(const std::filesystem::path& path) {
  return "blake3:" + svp::models::blake3_hex_for_file(path);
}

inline nlohmann::json write_valid_model_bundle(
    const std::filesystem::path& bundle_root,
    std::string_view model_id = "model_builder_preflight_test",
    std::string_view model_version = "1.0") {
  std::filesystem::create_directories(bundle_root);
  write_text(bundle_root / "model.onnx", "synthetic model bytes\n");
  write_text(bundle_root / "LICENSE", "synthetic test license\n");
  write_text(bundle_root / "NOTICE", "synthetic test notice\n");

  nlohmann::json manifest = {
      {"schema_version", "svp-model-bundle-1"},
      {"model_bundle_id",
       std::string(model_id) + "@" + std::string(model_version) +
           "+blake3_000000000000"},
      {"model_id", model_id},
      {"model_version", model_version},
      {"bundle_blake3",
       "blake3:0000000000000000000000000000000000000000000000000000000000000000"},
      {"runtime", "onnxruntime"},
      {"format", "onnx"},
      {"license", "Apache-2.0"},
      {"supported_execution_providers", nlohmann::json::array({"cpu"})},
      {"files",
       nlohmann::json::array(
           {{{"path", "model.onnx"},
             {"role", "model"},
             {"blake3", file_digest(bundle_root / "model.onnx")}},
            {{"path", "LICENSE"},
             {"role", "license"},
             {"blake3", file_digest(bundle_root / "LICENSE")}},
            {{"path", "NOTICE"},
             {"role", "notice"},
             {"blake3", file_digest(bundle_root / "NOTICE")}}})},
      {"input_contract", nlohmann::json::object()},
      {"output_contract", nlohmann::json::object()},
      {"preprocessor_contract", nlohmann::json::object()},
      {"postprocessor_contract", nlohmann::json::object()},
  };

  write_text(bundle_root / "model.svpmodel.json", manifest.dump(2) + "\n");
  const std::string digest =
      "blake3:" + svp::models::blake3_hex_for_model_bundle(bundle_root);
  manifest["bundle_blake3"] = digest;
  manifest["model_bundle_id"] =
      std::string(model_id) + "@" + std::string(model_version) +
      "+blake3_" + digest.substr(7, 12);
  write_text(bundle_root / "model.svpmodel.json", manifest.dump(2) + "\n");
  return manifest;
}

inline void write_model_lock(
    const std::filesystem::path& cache_root,
    const std::vector<nlohmann::json>& manifests) {
  nlohmann::json models = nlohmann::json::array();
  for (const nlohmann::json& manifest : manifests) {
    models.push_back({
        {"model_id", manifest.at("model_id")},
        {"model_bundle_id", manifest.at("model_bundle_id")},
        {"model_version", manifest.at("model_version")},
        {"bundle_blake3", manifest.at("bundle_blake3")},
        {"files", manifest.at("files")},
    });
  }
  const nlohmann::json lock = {
      {"schema_version", "svp-model-lock-1"},
      {"model_set_id", "builder-preflight-test"},
      {"models", std::move(models)},
  };
  write_text(cache_root / "model-lock.json", lock.dump(2) + "\n");
}

inline std::filesystem::path write_valid_model_cache(
    const std::filesystem::path& cache_root) {
  const nlohmann::json manifest = write_valid_model_bundle(
      cache_root / "bundle", "model_builder_preflight_test", "1.0");
  write_model_lock(cache_root, {manifest});
  return cache_root;
}

}  // namespace svp::builder::test
