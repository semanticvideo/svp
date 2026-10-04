#pragma once

// The audio stage work a --distributed build may dispatch (M5,
// DistributedAudioWork): the bundles its ASR chunks and diarization windows
// load, exactly as the audio stage resolves them (audio_stage.cpp,
// execute_asr_boundary: Whisper and VAD must verify, the phoneme aligner is
// used only when it verifies; diarization needs its bundle to verify). A
// kind is left out when its bundles
// are not in this Mac's cache or its thread counts are not explicit (workers
// refuse thread counts left to each Mac, plan §2.4 item 5); its stage then
// does its work itself.

#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/distributed_execution.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/thread_plan.hpp"

#include <filesystem>

namespace svp::builder::engine {

// The audio work a build with audio dispatches from `model_cache_root` and
// `thread_plan`, whatever its media: what `workers sync` measures ahead of
// the builds that will dispatch it.
[[nodiscard]] DistributedAudioWork plan_distributed_audio_models(
    const std::filesystem::path& model_cache_root, const svp::models::ThreadPlan& thread_plan);

// The same for one build: nothing when the build runs no audio stage, its
// media has no audio stream, or it has no model cache.
[[nodiscard]] DistributedAudioWork plan_distributed_audio_work(
    const BuildPipelineOptions& options, const BuildStageExecutionPlan& stage_plan,
    const svp::media::MediaIngestPlan& plan, const svp::models::ThreadPlan& thread_plan);

}  // namespace svp::builder::engine
