#include "svp/vision/tasks/model_refs.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/models/manifest.hpp"

#include <stdexcept>

namespace svp::vision::tasks {
namespace {

// The bundle manifest's file name in a model cache.
constexpr const char* kManifestFilename = "model.svpmodel.json";

}  // namespace

svp::exec::TaskModelRef cached_model_ref(const std::filesystem::path& model_cache_root,
                                         const std::string& model_id) {
  const std::filesystem::path manifest_path = model_cache_root / model_id / kManifestFilename;
  const svp::models::ModelBundleManifest manifest = [&] {
    try {
      return svp::models::load_model_bundle_manifest(manifest_path);
    } catch (const std::exception& error) {
      throw std::runtime_error("model ref for " + model_id + ": " + error.what());
    }
  }();
  const auto bundle_blake3 = svp::exec::parse_blake3_hex(manifest.bundle_blake3.hex_value());
  if (manifest.model_id != model_id || !bundle_blake3) {
    throw std::runtime_error("model ref for " + model_id + ": manifest " +
                             manifest_path.string() + " does not identify that model by BLAKE3");
  }
  return svp::exec::TaskModelRef{.model_id = manifest.model_id,
                                 .model_bundle_id = manifest.model_bundle_id,
                                 .bundle_blake3 = *bundle_blake3};
}

std::optional<std::string> single_model_ref_problem(const svp::exec::TaskSpec& spec,
                                                    const std::string& model_id) {
  if (spec.model_refs.size() != 1 || spec.model_refs.front().model_id != model_id) {
    return "model_refs must name exactly " + model_id;
  }
  return std::nullopt;
}

std::optional<std::string> loaded_bundle_mismatch(const svp::exec::TaskSpec& spec,
                                                  const std::string& model_id,
                                                  const std::string& loaded_bundle_blake3_hex) {
  const svp::exec::TaskModelRef& ref = spec.model_refs.front();
  if (ref.model_id != model_id ||
      svp::exec::blake3_hex(ref.bundle_blake3) != loaded_bundle_blake3_hex) {
    return "loaded bundle " + model_id + "@" + loaded_bundle_blake3_hex + " is not " +
           ref.model_bundle_id;
  }
  return std::nullopt;
}

}  // namespace svp::vision::tasks
