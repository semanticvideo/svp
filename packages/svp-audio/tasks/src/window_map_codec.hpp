#pragma once

#include "svp/audio/diarization_window_map.hpp"

#include <nlohmann/json.hpp>

namespace svp::audio::tasks {

// The diarize.window output body of a map (read_diarize_window_output is the
// inverse): pieces of speakers, each with its segments as [start, end, ...]
// seconds and its embedding, every float carried exactly.
[[nodiscard]] nlohmann::json diarize_window_map_json(const svp::audio::DiarizationWindowMap& map);

}  // namespace svp::audio::tasks
