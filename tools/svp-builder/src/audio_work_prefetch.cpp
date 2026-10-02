#include "audio_work_prefetch.hpp"

#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/whisper_cpp_model.hpp"

#include <utility>

namespace svp::builder {
namespace {

bool same_chunk(const svp::audio::AsrChunkPlan& left, const svp::audio::AsrChunkPlan& right) {
  return left.chunk_id == right.chunk_id && left.source_start_us == right.source_start_us &&
         left.source_end_us == right.source_end_us &&
         left.overlap_before_us == right.overlap_before_us &&
         left.overlap_after_us == right.overlap_after_us && left.input_ref == right.input_ref &&
         left.output_ref == right.output_ref && left.model_id == right.model_id &&
         left.runtime == right.runtime && left.asr_status == right.asr_status;
}

bool same_work(const svp::audio::AsrChunkWork& left, const svp::audio::AsrChunkWork& right) {
  if (left.input_wav != right.input_wav || left.model_id != right.model_id ||
      left.vad_model_id != right.vad_model_id ||
      left.alignment_model_id != right.alignment_model_id ||
      left.threads.whisper != right.threads.whisper ||
      left.threads.forced_alignment != right.threads.forced_alignment ||
      left.chunks.size() != right.chunks.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.chunks.size(); ++index) {
    if (!same_chunk(left.chunks[index], right.chunks[index])) {
      return false;
    }
  }
  return true;
}

bool same_work(const svp::audio::DiarizationWindowWork& left,
               const svp::audio::DiarizationWindowWork& right) {
  return left.wav_path == right.wav_path && left.sample_count == right.sample_count &&
         left.window_count == right.window_count &&
         left.settings.threads == right.settings.threads &&
         left.settings.compute_embeddings == right.settings.compute_embeddings;
}

}  // namespace

PrefetchedAsrChunks::PrefetchedAsrChunks(const engine::AudioWorkDispatch* dispatch,
                                         std::vector<svp::audio::AsrChunkWork> works,
                                         const engine::AudioProgress& on_progress)
    : dispatch_(dispatch), works_(std::move(works)) {
  if (dispatch_ != nullptr && dispatch_->asr_chunks && !works_.empty()) {
    results_ = dispatch_->asr_chunks(works_, on_progress);
  }
}

svp::audio::AsrChunkDispatch PrefetchedAsrChunks::boundary_dispatch() const {
  if (dispatch_ == nullptr) {
    return {};
  }
  const svp::audio::AsrChunkDispatch single = engine::single_asr_chunk_dispatch(dispatch_);
  return [this, single](const svp::audio::AsrChunkWork& work,
                        const engine::AudioProgress& on_progress)
             -> std::optional<std::vector<std::optional<svp::audio::AsrChunkOutcome>>> {
    if (results_) {
      for (std::size_t index = 0; index < works_.size(); ++index) {
        if (same_work(works_[index], work)) {
          return (*results_)[index];
        }
      }
    } else if (!works_.empty()) {
      // The run declined (no worker takes chunks, or this Mac could not
      // plan them): every boundary's chunks run here.
      return std::nullopt;
    }
    return single ? single(work, on_progress) : std::nullopt;
  };
}

PrefetchedDiarizationWindows::PrefetchedDiarizationWindows(
    const engine::AudioWorkDispatch* dispatch,
    std::vector<svp::audio::DiarizationWindowWork> works, const engine::AudioProgress& on_progress)
    : dispatch_(dispatch), works_(std::move(works)) {
  if (dispatch_ != nullptr && dispatch_->diarization_windows && !works_.empty()) {
    results_ = dispatch_->diarization_windows(works_, on_progress);
  }
}

svp::audio::DiarizationWindowDispatch PrefetchedDiarizationWindows::run_dispatch() const {
  if (dispatch_ == nullptr) {
    return {};
  }
  const svp::audio::DiarizationWindowDispatch single =
      engine::single_diarization_window_dispatch(dispatch_);
  return [this, single](const svp::audio::DiarizationWindowWork& work,
                        const svp::audio::DiarizationProgressCallback& on_progress)
             -> std::optional<std::vector<std::optional<svp::audio::DiarizationWindowMap>>> {
    if (results_) {
      for (std::size_t index = 0; index < works_.size(); ++index) {
        if (same_work(works_[index], work)) {
          return (*results_)[index];
        }
      }
    } else if (!works_.empty()) {
      return std::nullopt;
    }
    return single ? single(work, on_progress) : std::nullopt;
  };
}

std::optional<svp::audio::AsrChunkWork> predicted_asr_chunk_work(
    const svp::audio::AsrExecutionBoundary& boundary, const std::filesystem::path& staging_root,
    const std::filesystem::path& model_cache_root,
    const svp::audio::WhisperRuntimeThreads& threads) {
  if (!boundary.blockers.empty() || boundary.input_refs.size() != 1 ||
      boundary.input_refs.front().empty()) {
    return std::nullopt;
  }
  try {
    const std::optional<std::filesystem::path> vad =
        svp::audio::find_whisper_ggml_vad_model(model_cache_root / boundary.vad_model_id);
    if (!vad || !svp::audio::verify_asr_model_files(boundary.vad_model_id, model_cache_root)) {
      return std::nullopt;
    }
    const std::filesystem::path input_wav = staging_root / boundary.input_refs.front();
    if (!std::filesystem::exists(input_wav)) {
      return std::nullopt;
    }
    svp::audio::AsrChunkWork work{.input_wav = input_wav,
                                  .chunks = boundary.chunk_plan.chunks,
                                  .model_id = boundary.model_id,
                                  .vad_model_id = boundary.vad_model_id,
                                  .alignment_model_id = std::nullopt,
                                  .threads = threads};
    const bool aligner_present = std::filesystem::exists(
        model_cache_root / boundary.alignment_model_id / "model.svpmodel.json");
    if (aligner_present &&
        svp::audio::verify_asr_model_files(boundary.alignment_model_id, model_cache_root)) {
      work.alignment_model_id = boundary.alignment_model_id;
    }
    return work;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace svp::builder
