#include "audio_task_spec.hpp"

#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cache_key.hpp"
#include "svp/exec/parameters_digest.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace svp::audio::tasks::detail {
namespace {

std::string padded(std::uint64_t position) {
  std::ostringstream oss;
  oss << std::setw(6) << std::setfill('0') << position;
  return oss.str();
}

}  // namespace

std::string audio_task_id(std::string_view task_type, std::uint64_t run_ordinal,
                          std::uint64_t first, std::uint64_t last) {
  return "task." + std::string(task_type) + ".audio_" + padded(run_ordinal) + ".items_" +
         padded(first) + "_" + padded(last);
}

svp::exec::TaskSpec assemble_audio_task_spec(AudioSpecAssembly assembly) {
  svp::exec::TaskSpec spec;
  spec.build_session_id = std::move(assembly.build_session_id);
  spec.task_id = std::move(assembly.task_id);
  spec.task_type = std::move(assembly.task_type);
  spec.task_type_version = assembly.task_type_version;
  spec.model_refs = std::move(assembly.model_refs);
  std::sort(spec.model_refs.begin(), spec.model_refs.end(),
            [](const auto& left, const auto& right) { return left.model_id < right.model_id; });
  spec.inputs.emplace(std::string(kAudioTaskInput), assembly.audio);
  spec.parameters = std::move(assembly.parameters);
  spec.parameters_blake3 = svp::exec::compute_parameters_blake3(spec.parameters);
  nlohmann::json bundle_ids = nlohmann::json::array();
  for (const svp::exec::TaskModelRef& ref : spec.model_refs) {
    bundle_ids.push_back(ref.model_bundle_id);
  }
  spec.cache_key = svp::exec::compute_cache_key(nlohmann::json::array({
      spec.task_type,
      spec.task_type_version,
      svp::exec::blake3_hex(assembly.audio.blake3),
      std::move(bundle_ids),
      svp::exec::blake3_hex(spec.parameters_blake3),
  }));
  spec.resources = assembly.resources;
  svp::exec::validate_task_spec(spec);
  return spec;
}

}  // namespace svp::audio::tasks::detail
