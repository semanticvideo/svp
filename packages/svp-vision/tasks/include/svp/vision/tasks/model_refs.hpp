#pragma once

#include "svp/exec/task_spec.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision::tasks {

// The TaskModelRef of the bundle `model_id` in a model cache
// (<cache>/<model_id>/model.svpmodel.json): its id, bundle id, and bundle
// BLAKE3. Throws std::runtime_error when the manifest is missing, malformed,
// or names another model.
[[nodiscard]] svp::exec::TaskModelRef cached_model_ref(
    const std::filesystem::path& model_cache_root, const std::string& model_id);

// nullopt when `spec` names exactly one model, `model_id`; otherwise why
// not (a spec the coordinator could not have planned: permanent).
[[nodiscard]] std::optional<std::string> single_model_ref_problem(
    const svp::exec::TaskSpec& spec, const std::string& model_id);

// nullopt when the bundle this runtime loaded for `model_id` (its manifest's
// bundle BLAKE3, hex) is the one the spec's model ref names; otherwise why
// not (this worker holds another bundle: retryable elsewhere).
[[nodiscard]] std::optional<std::string> loaded_bundle_mismatch(
    const svp::exec::TaskSpec& spec, const std::string& model_id,
    const std::string& loaded_bundle_blake3_hex);

}  // namespace svp::vision::tasks
