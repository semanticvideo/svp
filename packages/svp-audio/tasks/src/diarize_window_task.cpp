#include "audio_task_execution.hpp"

#include "task_outputs.hpp"
#include "window_map_codec.hpp"

#include "svp/audio/diarization_window_map.hpp"
#include "svp/audio/tasks/diarize_window.hpp"

namespace svp::audio::tasks::detail {

svp::exec::TaskResult execute_diarize_window(const svp::exec::TaskSpec& spec,
                                             const svp::exec::ResolvedInputs& inputs,
                                             const svp::exec::CancellationToken& cancellation,
                                             const AudioTaskEnvironment& environment) {
  svp::exec::throw_if_cancelled(cancellation, "diarize.window start");
  const DiarizeWindowParameters parameters = diarize_window_parameters_from_json(spec.parameters);
  const auto audio = inputs.find(std::string(kAudioTaskInput));
  if (spec.inputs.size() != 1 || audio == inputs.end()) {
    return failed_result(spec, "invalid_inputs",
                         "inputs must be exactly `" + std::string(kAudioTaskInput) + "`", false);
  }
  if (spec.model_refs.size() != 1 || spec.model_refs.front().model_id != parameters.model_id) {
    return failed_result(spec, "invalid_model_refs",
                         "model_refs must name exactly " + parameters.model_id, false);
  }

  // A window that cannot be mapped here: on a worker the task fails
  // retryably, so another Mac maps it; on the coordinator it is recorded as
  // not mapped, and diarization maps it itself exactly as a build without
  // tasks does.
  const auto not_mapped = [&](std::string code, std::string reason) {
    if (!environment.record_start_failures) {
      return failed_result(spec, std::move(code), std::move(reason), true);
    }
    return cbor_result(spec, environment,
                       {{"not_mapped", reason}, {"window_index", parameters.window_index}},
                       kDiarizeWindowMapMediaType, kDiarizeWindowMapRole, {{"mapped", false}});
  };

  const std::optional<std::string> library = loaded_sherpa_library_identity();
  if (!library) {
    return not_mapped("runtime_unavailable", "sherpa-onnx does not load here");
  }
  if (*library != parameters.sherpa_library) {
    return not_mapped("runtime_mismatch", "sherpa-onnx here is " + *library +
                                              ", the coordinator loaded " +
                                              parameters.sherpa_library);
  }
  std::filesystem::path model_cache_root = environment.model_cache_root;
  if (environment.model_cache_for) {
    try {
      model_cache_root = environment.model_cache_for(spec);
    } catch (const std::exception& error) {
      return not_mapped("model_unavailable", error.what());
    }
  }
  std::size_t sample_count = 0;
  try {
    sample_count = svp::audio::diarization_wav_sample_count(audio->second.path);
  } catch (const std::exception& error) {
    return failed_result(spec, "invalid_inputs", error.what(), false);
  }
  if (sample_count != parameters.sample_count) {
    return failed_result(spec, "invalid_inputs",
                         "the WAV has " + std::to_string(sample_count) + " samples, not " +
                             std::to_string(parameters.sample_count),
                         false);
  }

  const svp::audio::DiarizationWindowOutcome outcome = svp::audio::run_diarization_window_map(
      audio->second.path, model_cache_root / parameters.model_id,
      static_cast<std::size_t>(parameters.window_index),
      svp::audio::DiarizationWindowSettings{.threads = parameters.threads,
                                            .compute_embeddings = parameters.compute_embeddings},
      [&cancellation] {
        svp::exec::throw_if_cancelled(cancellation, "diarize.window between pieces");
      });
  svp::exec::throw_if_cancelled(cancellation, "diarize.window before output");
  if (outcome.failure || !outcome.map) {
    return not_mapped("window_failed_here",
                      outcome.failure ? outcome.failure->blocker : "no map");
  }
  return cbor_result(spec, environment, diarize_window_map_json(*outcome.map),
                     kDiarizeWindowMapMediaType, kDiarizeWindowMapRole,
                     {{"mapped", true}, {"pieces", outcome.map->pieces.size()}});
}

}  // namespace svp::audio::tasks::detail
