#include "engine/distributed_audio_work.hpp"

#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/diarization_boundary.hpp"
#include "svp/models/reference_processor_model_ids.hpp"
#include "svp/vision/tasks/model_refs.hpp"

namespace svp::builder::engine {
namespace {

std::optional<svp::exec::TaskModelRef> verified_ref(const std::filesystem::path& model_cache_root,
                                                    const std::string& model_id) {
  try {
    if (!svp::audio::verify_asr_model_files(model_id, model_cache_root)) {
      return std::nullopt;
    }
    return svp::vision::tasks::cached_model_ref(model_cache_root, model_id);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

DistributedAudioWork plan_distributed_audio_work(const BuildPipelineOptions& options,
                                                 const BuildStageExecutionPlan& stage_plan,
                                                 const svp::media::MediaIngestPlan& plan,
                                                 const svp::models::ThreadPlan& thread_plan) {
  DistributedAudioWork work;
  if (!stage_plan.run_audio || plan.probe.audio_streams.empty() ||
      options.model_cache_dir.empty()) {
    return work;
  }
  const std::filesystem::path cache(options.model_cache_dir);
  const bool asr_threads = thread_plan.whisper.decode >= 1 && thread_plan.whisper.vad >= 1 &&
                           thread_plan.forced_alignment.intra_op >= 1 &&
                           thread_plan.forced_alignment.inter_op >= 1;
  const auto whisper = verified_ref(cache, svp::models::kWhisperSmallEnglishModelId);
  const auto vad = verified_ref(cache, svp::models::kWhisperCppSileroVadModelId);
  if (asr_threads && whisper && vad) {
    work.asr_model_refs = {*whisper, *vad};
    if (const auto aligner = verified_ref(cache, svp::models::kWav2Vec2EspeakPhonemeModelId)) {
      work.asr_model_refs.push_back(*aligner);
    }
  }
  const bool sherpa_threads =
      thread_plan.sherpa.segmentation >= 1 && thread_plan.sherpa.embedding >= 1;
  // Under --force-single-speaker the single-stream path never diarizes, but
  // the microphone path still fingerprints each stream, so the bundle is
  // named either way.
  if (sherpa_threads &&
      svp::audio::verify_diarization_model_files(svp::models::kSherpaOnnxDiarizationModelId,
                                                 cache)) {
    try {
      work.diarization_model_ref = svp::vision::tasks::cached_model_ref(
          cache, svp::models::kSherpaOnnxDiarizationModelId);
    } catch (const std::exception&) {
      work.diarization_model_ref.reset();
    }
  }
  return work;
}

}  // namespace svp::builder::engine
