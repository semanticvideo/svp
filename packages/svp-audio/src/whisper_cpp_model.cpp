#include "svp/audio/whisper_cpp_model.hpp"

#include "svp/models/manifest.hpp"

#include <algorithm>
#include <vector>

namespace svp::audio {

std::optional<std::filesystem::path> find_whisper_ggml_model(
    const std::filesystem::path& model_dir) {
  if (!std::filesystem::is_directory(model_dir)) return std::nullopt;

  const std::filesystem::path manifest_path =
      model_dir / "model.svpmodel.json";
  if (!std::filesystem::is_regular_file(manifest_path)) return std::nullopt;
  const svp::models::ModelBundleManifest manifest =
      svp::models::load_model_bundle_manifest(manifest_path);
  if (manifest.runtime != "whisper.cpp" || manifest.format != "ggml") {
    return std::nullopt;
  }

  std::vector<std::filesystem::path> candidates;
  for (const auto& file : manifest.files) {
    const std::filesystem::path path = model_dir / file.path;
    if (!std::filesystem::is_regular_file(path)) continue;
    const std::string filename = path.filename().string();
    if (path.extension() == ".bin" && filename.starts_with("ggml-")) {
      candidates.push_back(std::filesystem::weakly_canonical(path));
    }
  }
  std::sort(candidates.begin(), candidates.end());
  if (candidates.size() != 1) return std::nullopt;
  return candidates.front();
}

}  // namespace svp::audio
