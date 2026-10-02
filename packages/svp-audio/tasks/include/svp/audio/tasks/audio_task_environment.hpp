#pragma once

// What a runtime gives the audio task types (asr.chunk_batch,
// diarize.window) beyond their TaskSpecs: its own model cache (never sent by
// the coordinator, plan §4.3: no message carries a path), a scratch
// directory, and the store their outputs go to.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/exec/task_spec.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace svp::audio::tasks {

// The analysis WAV a task reads: TaskSpec input name and artifact role.
inline constexpr std::string_view kAudioTaskInput = "audio";
inline constexpr std::string_view kAudioTaskInputRole = "analysis_audio";
inline constexpr std::string_view kAudioTaskInputMediaType = "audio/wav";

struct AudioTaskEnvironment {
  // The model cache tasks load from, unless model_cache_for is set.
  std::filesystem::path model_cache_root;
  // Optional: the model cache for one spec (a worker's view over exactly the
  // bundles the spec's model_refs name). Throwing means this runtime cannot
  // provide those bundles now.
  std::function<std::filesystem::path(const svp::exec::TaskSpec& spec)> model_cache_for;
  // Where chunk slices are written (each task makes its own directory in it).
  std::filesystem::path scratch_dir;
  // Stores output bytes in the runtime's artifact store and returns their ref.
  std::function<svp::exec::ArtifactRef(std::span<const std::byte> bytes, std::string media_type,
                                       std::string role)>
      write_output;
  // True only on the coordinator: an item that cannot run here (a model or
  // library that does not load, whisper or sherpa-onnx failing) is recorded
  // as not done, so the stage runs it itself exactly as a build without
  // tasks does. False (workers): a retryable failure, so the item runs on
  // another Mac and output never depends on which Mac took it.
  bool record_start_failures = false;
};

// Registers asr.chunk_batch and diarize.window.
void register_audio_tasks(svp::exec::TaskTypeRegistry& registry,
                          AudioTaskEnvironment environment);

}  // namespace svp::audio::tasks
