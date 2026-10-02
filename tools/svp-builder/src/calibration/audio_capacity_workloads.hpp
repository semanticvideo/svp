#pragma once

// The capacity calibration workloads of the dispatched audio task types
// (capacity_sweep.hpp; plan §3.5 "each worker measures throughput per task
// type"), run on one speech clip every Mac decodes the same way, so
// measurements of different Macs compare like for like.
//
// The clip is synthesized speech: the coordinator renders a fixed English
// passage with macOS's speech synthesizer (/usr/bin/say, part of every macOS
// install) and converts it with its ffmpeg to the 16 kHz mono PCM16 analysis
// format; workers receive the coordinator's clip bytes, never render their
// own. The passage lasts about half a minute, so it spans several ASR chunks
// of the fixed grid and stays inside one diarization window:
//   * asr.chunk_batch: every chunk of the clip's chunk plan (one item each);
//   * diarize.window: the clip's single window, one item per whole second of
//     audio, so seconds per item is seconds per second of window audio (what
//     a window's lease estimate scales).
// The clip's bytes are part of each type's calibration conditions (the
// rendering depends on the synthesizer's voice), so another clip is measured
// again.

#include "calibration/capacity_sweep.hpp"

#include "svp/builder/distributed_execution.hpp"
#include "svp/exec/artifact_ref.hpp"
#include "svp/models/thread_plan.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::calibration {

// Bumped whenever the passage, its rendering, or a workload below changes.
inline constexpr std::uint32_t kAudioCalibrationRecipeVersion = 1;

// Renders the passage with `say` and converts it with `ffmpeg` into
// `directory/speech-calibration.wav`. Throws std::runtime_error when either
// fails.
[[nodiscard]] std::filesystem::path write_speech_calibration_clip(
    const std::filesystem::path& ffmpeg, const std::filesystem::path& directory);

struct AudioCalibrationSetup {
  DistributedAudioWork audio;
  svp::models::ThreadPlan thread_plan;
  // b3:<hex> of the sherpa-onnx library the calibration's diarize.window
  // tasks must load (svp::audio::tasks::loaded_sherpa_library_identity on
  // the measuring side's process).
  std::string sherpa_library;
};

// The audio task types `audio` names, in a fixed order.
[[nodiscard]] std::vector<std::string> audio_task_types(const DistributedAudioWork& audio);

// Peak RSS of one task of `task_type` (its spec header's estimate).
[[nodiscard]] std::uint64_t audio_task_peak_rss_mb(std::string_view task_type);

// What a type's measurement depends on besides the Mac and the runtime: its
// models, thread counts, recipe, and the clip's bytes.
[[nodiscard]] nlohmann::json audio_capacity_settings(std::string_view task_type,
                                                     const AudioCalibrationSetup& setup,
                                                     const svp::exec::ArtifactRef& clip);

// The calibration workload of `task_type` on the clip `clip` (role
// kAudioTaskInputRole), whose file is `clip_file`. Throws std::runtime_error
// when `setup` does not dispatch the type or the clip cannot be read.
[[nodiscard]] CapacityWorkload audio_capacity_workload(std::string_view task_type,
                                                       const AudioCalibrationSetup& setup,
                                                       const svp::exec::ArtifactRef& clip,
                                                       const std::filesystem::path& clip_file);

}  // namespace svp::builder::calibration
