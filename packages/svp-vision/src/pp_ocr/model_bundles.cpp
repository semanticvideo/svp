#include "pp_ocr_internal.hpp"

#include "svp/models/hash.hpp"

#include <filesystem>
#include <fstream>

namespace svp::vision::pp_ocr_internal {

std::vector<std::string> load_char_dict_from_yaml(
    const std::filesystem::path& yml_path) {
  std::vector<std::string> chars;
  std::ifstream in(yml_path);
  if (!in) return chars;

  std::string line;
  bool in_dict_section = false;
  while (std::getline(in, line)) {
    if (line.find("character_dict:") != std::string::npos) {
      in_dict_section = true;
      continue;
    }
    if (in_dict_section) {
      auto trim_start = line.find_first_not_of(" \t");
      if (trim_start == std::string::npos) continue;
      if (line[trim_start] != '-') {
        if (line.find(":") != std::string::npos && trim_start <= 2) break;
        continue;
      }
      std::string item = line.substr(trim_start + 1);
      auto item_start = item.find_first_not_of(" \t");
      if (item_start == std::string::npos) continue;
      item = item.substr(item_start);

      if (item.size() >= 2 && item.front() == '\'' && item.back() == '\'') {
        item = item.substr(1, item.size() - 2);
      } else if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
        item = item.substr(1, item.size() - 2);
      }

      if (item == "\\n") item = "\n";
      else if (item == "\\t") item = "\t";
      else if (item == "\\\\") item = "\\";

      chars.push_back(item);
    }
  }
  return chars;
}

bool verify_file_hash(
    const svp::models::ModelBundleManifest& manifest,
    const std::filesystem::path& bundle_dir,
    const std::string& filename) {
  for (const auto& mf : manifest.files) {
    if (mf.path == filename) {
      const auto file_path = bundle_dir / filename;
      if (!std::filesystem::exists(file_path)) return false;
      std::string computed;
      try {
        computed = svp::models::blake3_hex_for_file(file_path);
      } catch (...) {
        return false;
      }
      return computed == mf.blake3.hex_value();
    }
  }
  return false;
}

std::optional<ModelBundlePaths> find_pp_ocr_bundles(
    const PpOcrOptions& options) {
  ModelBundlePaths paths;

  const std::vector<std::string> det_dir_names = {
      options.detector_model_id};
  const std::vector<std::string> rec_dir_names = {
      options.recognizer_model_id};

  for (const auto& name : det_dir_names) {
    auto dir = options.model_cache_root / name;
    if (std::filesystem::exists(dir)) {
      paths.det_bundle_dir = dir;
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto p = entry.path();
        if (p.extension() == ".onnx") paths.det_onnx = p;
        if (p.extension() == ".yml" || p.extension() == ".yaml") paths.det_yml = p;
      }
      auto manifest_path = dir / options.manifest_filename;
      if (std::filesystem::exists(manifest_path)) {
        try {
          auto manifest = svp::models::load_model_bundle_manifest(manifest_path);
          paths.det_model_id = manifest.model_id;
          paths.det_model_version = manifest.model_version;
          paths.det_bundle_blake3 = manifest.bundle_blake3.hex_value();
          if (!manifest.license.empty()) paths.license = manifest.license;
          paths.det_manifest = std::move(manifest);
          paths.det_manifest_loaded = true;
        } catch (...) {}
      }
      if (paths.det_model_id.empty()) paths.det_model_id = name;
      break;
    }
  }

  for (const auto& name : rec_dir_names) {
    auto dir = options.model_cache_root / name;
    if (std::filesystem::exists(dir)) {
      paths.rec_bundle_dir = dir;
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto p = entry.path();
        if (p.extension() == ".onnx") paths.rec_onnx = p;
        if (p.extension() == ".yml" || p.extension() == ".yaml") paths.rec_yml = p;
      }
      auto manifest_path = dir / options.manifest_filename;
      if (std::filesystem::exists(manifest_path)) {
        try {
          auto manifest = svp::models::load_model_bundle_manifest(manifest_path);
          paths.rec_model_id = manifest.model_id;
          paths.rec_model_version = manifest.model_version;
          paths.rec_bundle_blake3 = manifest.bundle_blake3.hex_value();
          if (!manifest.license.empty() && paths.license.empty()) {
            paths.license = manifest.license;
          }
          paths.rec_manifest = std::move(manifest);
          paths.rec_manifest_loaded = true;
        } catch (...) {}
      }
      if (paths.rec_model_id.empty()) paths.rec_model_id = name;
      break;
    }
  }

  if (paths.det_onnx.empty() || paths.rec_onnx.empty()) return std::nullopt;
  if (paths.license.empty()) paths.license = "Apache-2.0";
  return paths;
}

}  // namespace svp::vision::pp_ocr_internal
