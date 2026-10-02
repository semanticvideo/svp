#include "audio_task_execution.hpp"

#include "task_outputs.hpp"

#include "svp/audio/asr_slice_workspace.hpp"
#include "svp/audio/tasks/asr_chunk_batch.hpp"

#include <set>

namespace svp::audio::tasks::detail {
namespace {

nlohmann::json word_json(const svp::audio::AsrWord& word) {
  return nlohmann::json::array({word.text, word.start_us, word.end_us, word.confidence,
                                word.chunk_ordinal, word.timing_source});
}

svp::exec::TaskResult outcomes_result(const svp::exec::TaskSpec& spec,
                                      const AudioTaskEnvironment& environment,
                                      nlohmann::json records, std::size_t ran) {
  return cbor_result(spec, environment, nlohmann::json{{"chunks", std::move(records)}},
                     kAsrChunkOutcomesMediaType, kAsrChunkOutcomesRole,
                     {{"chunks", spec.parameters.at("chunks").size()}, {"ran", ran}});
}

}  // namespace

svp::exec::TaskResult execute_asr_chunk_batch(const svp::exec::TaskSpec& spec,
                                              const svp::exec::ResolvedInputs& inputs,
                                              const svp::exec::CancellationToken& cancellation,
                                              const AudioTaskEnvironment& environment) {
  svp::exec::throw_if_cancelled(cancellation, "asr.chunk_batch start");
  const AsrChunkBatchParameters parameters = asr_chunk_batch_parameters_from_json(spec.parameters);
  const auto audio = inputs.find(std::string(kAudioTaskInput));
  if (spec.inputs.size() != 1 || audio == inputs.end()) {
    return failed_result(spec, "invalid_inputs",
                         "inputs must be exactly `" + std::string(kAudioTaskInput) + "`", false);
  }
  std::set<std::string> named{parameters.model_id, parameters.vad_model_id};
  if (parameters.alignment_model_id) {
    named.insert(*parameters.alignment_model_id);
  }
  std::set<std::string> referenced;
  for (const svp::exec::TaskModelRef& ref : spec.model_refs) {
    referenced.insert(ref.model_id);
  }
  if (referenced != named || spec.model_refs.size() != named.size()) {
    return failed_result(spec, "invalid_model_refs",
                         "model_refs must name exactly the Whisper, VAD, and aligner models",
                         false);
  }

  // A chunk that cannot run here: on a worker the task fails retryably, so
  // another Mac runs it; on the coordinator it is recorded as not run, and
  // the ASR boundary runs it itself exactly as a build without tasks does.
  const auto could_not_start = [&](std::string code, std::string reason) {
    if (!environment.record_start_failures) {
      return failed_result(spec, std::move(code), std::move(reason), true);
    }
    nlohmann::json records = nlohmann::json::array();
    for (const AsrChunkBatchItem& item : parameters.chunks) {
      records.push_back({{"not_run", reason}, {"ordinal", item.ordinal}});
    }
    return outcomes_result(spec, environment, std::move(records), 0);
  };

  std::filesystem::path model_cache_root = environment.model_cache_root;
  if (environment.model_cache_for) {
    try {
      model_cache_root = environment.model_cache_for(spec);
    } catch (const std::exception& error) {
      return could_not_start("model_unavailable", error.what());
    }
  }
  std::string why;
  const std::optional<svp::audio::AsrChunkModels> models = svp::audio::resolve_asr_chunk_models(
      model_cache_root, parameters.model_id, parameters.vad_model_id,
      parameters.alignment_model_id, why);
  if (!models) {
    return could_not_start("model_unavailable", why);
  }

  std::optional<svp::audio::AsrSliceWorkspace> slices;
  try {
    slices.emplace(environment.scratch_dir);
  } catch (const std::exception& error) {
    return could_not_start("scratch_unavailable", error.what());
  }

  nlohmann::json records = nlohmann::json::array();
  std::size_t ran = 0;
  for (const AsrChunkBatchItem& item : parameters.chunks) {
    svp::exec::throw_if_cancelled(cancellation, "asr.chunk_batch between chunks");
    std::optional<svp::audio::AsrChunkOutcome> outcome;
    std::string problem;
    try {
      outcome = svp::audio::run_asr_chunk(audio->second.path, item.chunk,
                                          static_cast<std::int64_t>(item.ordinal), *models,
                                          parameters.threads, slices->path());
      if (!outcome->ran) {
        problem = outcome->blockers.empty() ? "whisper did not run" : outcome->blockers.front();
      } else if (!outcome->alignment_error.empty() && !environment.record_start_failures) {
        // The aligner failed here (a host condition, not the chunk's): the
        // coordinator keeps such a fallback as a local build does, a worker
        // lets another Mac align the chunk.
        problem = "phoneme alignment failed: " + outcome->alignment_error;
      }
    } catch (const std::exception& error) {
      problem = error.what();
    }
    if (!problem.empty()) {
      if (!environment.record_start_failures) {
        return failed_result(spec, "chunk_failed_here",
                             item.chunk.chunk_id + ": " + problem, true);
      }
      records.push_back({{"not_run", problem}, {"ordinal", item.ordinal}});
      continue;
    }
    nlohmann::json words = nlohmann::json::array();
    for (const svp::audio::AsrWord& word : outcome->words) {
      words.push_back(word_json(word));
    }
    records.push_back({{"alignment_status", outcome->alignment_status},
                       {"decoded_word_count", outcome->decoded_word_count},
                       {"ordinal", item.ordinal},
                       {"words", std::move(words)}});
    ++ran;
  }
  svp::exec::throw_if_cancelled(cancellation, "asr.chunk_batch before output");
  return outcomes_result(spec, environment, std::move(records), ran);
}

}  // namespace svp::audio::tasks::detail
