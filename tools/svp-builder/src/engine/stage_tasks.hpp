#pragma once

// The whole-stage task functions: each runs one stage of today's pipeline in
// this process, through the existing stage code, and returns its states. The
// registry (stage_task_registry.hpp) captures each task's staging scope.

#include "engine/stage_task_environment.hpp"
#include "engine/stage_task_plan.hpp"

namespace svp::builder::engine {

[[nodiscard]] StageStates run_stage_task(StageTaskKind kind,
                                         const StageTaskEnvironment& environment);

// Per-lane task functions (stage_tasks_*.cpp).
[[nodiscard]] StageStates run_inventory_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_color_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_vision_plan_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_foundation_ocr_task(const StageTaskEnvironment& environment);

[[nodiscard]] StageStates run_audio_extract_task(const StageTaskEnvironment& environment);
// Throws StageExitError when the audio lane must end the build.
[[nodiscard]] StageStates run_audio_transcribe_task(const StageTaskEnvironment& environment);

[[nodiscard]] StageStates run_canonical_frames_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_depth_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_ocr_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_text_embeddings_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_tracking_task(const StageTaskEnvironment& environment);

[[nodiscard]] StageStates run_entities_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_relationships_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_index_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_validation_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_package_write_task(const StageTaskEnvironment& environment);

[[nodiscard]] StageStates run_media_binding_task(const StageTaskEnvironment& environment);
[[nodiscard]] StageStates run_svpi_write_task(const StageTaskEnvironment& environment);

// The committed SVPI write task's outcome.
[[nodiscard]] SvpiPublicationResult svpi_publication_result(
    const CommittedStageResults& results);

}  // namespace svp::builder::engine
