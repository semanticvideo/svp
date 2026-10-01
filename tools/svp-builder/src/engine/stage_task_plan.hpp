#pragma once

// Plans a build as whole-stage tasks (RC2 §5.16, §20.1-§20.2): one task per
// stage of today's pipeline, with dependencies that reproduce today's stage
// order and lane concurrency exactly.
//
// Package build, lanes overlapping (the default):
//
//   inventory ─ color ─┬─ audio.extract ─ transcript ───────────────────┐
//                      │        └──────────────┐ (processors.jsonl)     │
//                      └─ frames ─┬─ depth ────┼─ tracking ─┐           │
//                                 └─ ocr ─ embedding.text ┘ └─ entities ┴─
//      ─ relationships ─ index ─ validation ─ package.write
//      [─ binding ─ svpi.write]                         (interlace create)
//
// The audio and vision lanes overlap only when the concurrency policy allows
// two heavy lanes; otherwise the vision lane waits for the audio lane, as
// before. `--serial` turns the whole build into one chain in today's serial
// order (vision lane, then audio lane). Within the vision lane depth runs
// beside OCR unless the build is serial, as before.
//
// Staging scopes (staging_scope.hpp) of tasks that may run at the same time
// never overlap; plan_stage_tasks checks this for every pair. Audio
// extraction rewrites provenance/processors.jsonl, so tracking, which merges
// into that file, is ordered after it (formerly guaranteed only by timing);
// the microphone path appends to it after ASR, so with several microphones
// tracking also waits for transcription.

#include "engine/staging_scope.hpp"

#include "svp/builder/build_pipeline.hpp"
#include "svp/exec/task_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::engine {

// Every whole-stage task type is at version 1: the stage code it wraps is the
// versioned unit, and the build inputs digest covers the builder version.
inline constexpr std::uint64_t kStageTaskTypeVersion = 1;

enum class StageTaskKind {
  inventory,
  color,
  vision_plan,
  foundation_ocr,
  audio_extract,
  audio_transcribe,
  canonical_frames,
  depth,
  ocr,
  text_embeddings,
  tracking,
  entities,
  relationships,
  index,
  validation,
  package_write,
  media_binding,
  svpi_write,
};

// RC2 §20.2-style task ID ("task.depth.vstream_000.canonical") and lowercase
// task type ("depth.canonical_frames").
[[nodiscard]] std::string_view stage_task_id(StageTaskKind kind) noexcept;
[[nodiscard]] std::string_view stage_task_type(StageTaskKind kind) noexcept;

struct PlannedStageTask {
  StageTaskKind kind = StageTaskKind::inventory;
  std::vector<StageTaskKind> depends_on;
  StagingScope scope;
};

struct StageTaskPlanInputs {
  BuildStageExecutionPlan stage_plan;
  bool serial_pipeline = false;
  // From the builder concurrency policy: more than one lets the audio and
  // vision lanes overlap.
  std::size_t single_video_heavy_lanes = 1;
  // The audio plan has more than one microphone analysis stream.
  bool microphone_stream_mode = false;
  // interlace create: media binding and SVPI write follow the package.
  bool svpi_publication = false;
};

// Tasks in a topological order that is also today's serial stage order.
// Throws std::logic_error when two tasks that may run concurrently have
// overlapping staging scopes.
[[nodiscard]] std::vector<PlannedStageTask> plan_stage_tasks(
    const StageTaskPlanInputs& inputs);

// The TaskGraph for `tasks`. Every task's parameters hold
// `build_inputs_blake3` (build_inputs_digest.hpp) and its staging scope, so
// its cache key changes whenever the build's inputs or options do and a
// journal from different inputs is never resumed.
[[nodiscard]] svp::exec::TaskGraph make_stage_task_graph(
    const std::vector<PlannedStageTask>& tasks, const std::string& build_session_id,
    const std::string& build_inputs_blake3);

// Largest set of tasks no dependency path orders (the graph's width): the
// most tasks that can ever run at once. The in-process executor gets this
// many slots, so the graph's edges, which encode the concurrency policy, are
// the only limit on concurrency.
[[nodiscard]] std::size_t stage_task_graph_width(
    const std::vector<PlannedStageTask>& tasks);

}  // namespace svp::builder::engine
