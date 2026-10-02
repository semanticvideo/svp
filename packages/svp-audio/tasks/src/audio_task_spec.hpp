#pragma once

// The parts of an audio TaskSpec both audio task types build the same way:
// task IDs over a run's WAVs and item ranges, and the RC2 §20.3 cache key
// over the type, version, the WAV's bytes, the model bundles, and the
// parameters.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_spec.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::audio::tasks::detail {

// "task.<type>.audio_<run>.items_<first>_<last>", positions six digits
// (minimum), last inclusive: stable for a given plan.
[[nodiscard]] std::string audio_task_id(std::string_view task_type, std::uint64_t run_ordinal,
                                        std::uint64_t first, std::uint64_t last);

struct AudioSpecAssembly {
  std::string build_session_id;
  std::string task_type;
  std::uint64_t task_type_version = 0;
  std::string task_id;
  std::vector<svp::exec::TaskModelRef> model_refs;
  // The WAV (input kAudioTaskInput).
  svp::exec::ArtifactRef audio;
  nlohmann::json parameters;
  svp::exec::TaskResources resources;
};

// The validated TaskSpec. Throws what validate_task_spec throws.
[[nodiscard]] svp::exec::TaskSpec assemble_audio_task_spec(AudioSpecAssembly assembly);

}  // namespace svp::audio::tasks::detail
