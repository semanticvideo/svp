#include "engine/audio_work_dispatch.hpp"

#include "engine/stage_output_access.hpp"
#include "engine/subtask_run.hpp"

#include "svp/audio/audio_dispatch_error.hpp"
#include "svp/audio/tasks/asr_chunk_batch.hpp"
#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/audio/tasks/diarize_window.hpp"
#include "svp/exec/source_fingerprint.hpp"
#include "svp/models/reference_processor_model_ids.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace svp::builder::engine {
namespace {

namespace tasks = svp::audio::tasks;

std::optional<svp::exec::TaskModelRef> model_ref(const VisionDispatchSetup& setup,
                                                 const std::string& model_id) {
  const auto found =
      std::find_if(setup.model_refs.begin(), setup.model_refs.end(),
                   [&](const svp::exec::TaskModelRef& ref) { return ref.model_id == model_id; });
  if (found == setup.model_refs.end()) {
    return std::nullopt;
  }
  return *found;
}

// Names `wav` by its bytes, makes it resolvable by this Mac's slots, and
// adds it to the inputs every worker session is supplied (once per file).
svp::exec::ArtifactRef audio_input(const std::filesystem::path& wav, StageOutputAccess& artifacts,
                                   std::vector<DispatchedInput>& inputs) {
  const svp::exec::SourceFingerprintRecord fingerprint =
      svp::exec::fingerprint_source("analysis_audio", wav);
  const svp::exec::ArtifactRef ref{.blake3 = fingerprint.blake3,
                                   .bytes = fingerprint.size_bytes,
                                   .media_type = std::string(tasks::kAudioTaskInputMediaType),
                                   .role = std::string(tasks::kAudioTaskInputRole)};
  artifacts.register_input(ref, wav);
  if (std::none_of(inputs.begin(), inputs.end(),
                   [&](const DispatchedInput& input) { return input.ref.blake3 == ref.blake3; })) {
    inputs.push_back(DispatchedInput{.ref = ref, .file = wav});
  }
  return ref;
}

// Where each task ran, one line per executor (stderr): the items of each
// task it committed, as "<work>:<first>-<last>".
void report_placement(const VisionDispatchSetup& setup, std::string_view task_type,
                      const std::vector<svp::exec::CommittedResult>& results,
                      const std::vector<std::string>& item_labels) {
  if (!setup.report) {
    return;
  }
  std::map<std::string, std::vector<std::string>> by_executor;
  for (std::size_t index = 0; index < results.size(); ++index) {
    by_executor[results[index].executor_id].push_back(item_labels[index]);
  }
  for (const auto& [executor, labels] : by_executor) {
    std::ostringstream line;
    line << "svp-builder: " << task_type << " on " << executor << ":";
    for (const std::string& label : labels) {
      line << " " << label;
    }
    std::cerr << line.str() << "\n";
  }
}

std::vector<svp::exec::CommittedResult> run_releasing(const SubtaskRunRequest& request,
                                                      const VisionDispatchSetup& setup,
                                                      const svp::exec::TaskTypeRegistry& registry,
                                                      StageOutputAccess& artifacts) {
  // This Mac's idle models go once the tasks are done, whatever the
  // outcome.
  struct Release {
    const std::function<void()>& release;
    ~Release() {
      if (release) release();
    }
  } release{setup.release_idle_models};
  return run_subtasks(request, setup, registry, artifacts);
}

// Every exception leaving a dispatcher leaves it as AudioDispatchError, its
// message kept: the audio boundaries rethrow only that and turn anything
// else into a blocker, and a dispatcher that cannot deliver must fail the
// build rather than change the package.
template <typename Function>
Function guarded(std::string task_type, Function inner) {
  return [task_type = std::move(task_type), inner = std::move(inner)](const auto&... arguments) {
    try {
      return inner(arguments...);
    } catch (const svp::audio::AudioDispatchError&) {
      throw;
    } catch (const std::exception& error) {
      throw svp::audio::AudioDispatchError(task_type + ": " + error.what());
    } catch (...) {
      throw svp::audio::AudioDispatchError(task_type + ": unknown failure");
    }
  };
}

// Plans with `plan`; nullopt, after saying why, when the work cannot be
// planned: the stage then does it itself, exactly as a local build does.
template <typename Plan>
auto plan_or_skip(const VisionDispatchSetup& setup, std::string_view task_type, Plan plan)
    -> std::optional<decltype(plan())> {
  try {
    return plan();
  } catch (const std::exception& error) {
    if (setup.report) {
      std::cerr << "svp-builder: warning: " << task_type
                << " work runs in its stage on this Mac: " << error.what() << "\n";
    }
    return std::nullopt;
  }
}

struct PlannedRun {
  std::vector<svp::exec::TaskNode> nodes;
  std::vector<DispatchedInput> inputs;
  // Per node: its work, first item, item count, and progress units.
  std::vector<std::size_t> work_of_node;
  std::vector<std::size_t> first_of_node;
  std::vector<std::size_t> items_of_node;
  std::vector<std::size_t> units_of_node;
  std::size_t total_units = 0;
};

std::vector<std::string> item_labels(const PlannedRun& run) {
  std::vector<std::string> labels;
  for (std::size_t index = 0; index < run.nodes.size(); ++index) {
    const std::size_t first = run.first_of_node[index];
    labels.push_back(std::to_string(run.work_of_node[index]) + ":" + std::to_string(first) +
                     (run.items_of_node[index] > 1
                          ? "-" + std::to_string(first + run.items_of_node[index] - 1)
                          : std::string()));
  }
  return labels;
}

std::vector<svp::exec::CommittedResult> run_planned(const PlannedRun& run,
                                                    std::string_view task_type,
                                                    const AudioProgress& on_progress,
                                                    const VisionDispatchSetup& setup,
                                                    const svp::exec::TaskTypeRegistry& registry,
                                                    StageOutputAccess& artifacts) {
  std::map<std::string, std::size_t> units_by_task;
  for (std::size_t index = 0; index < run.nodes.size(); ++index) {
    units_by_task.emplace(run.nodes[index].spec.task_id, run.units_of_node[index]);
  }
  std::size_t done = 0;
  if (on_progress) {
    on_progress(0, run.total_units);
  }
  const SubtaskRunRequest request{.task_type = std::string(task_type),
                                  .nodes = run.nodes,
                                  .on_committed =
                                      [&](const svp::exec::TaskNode& node) {
                                        done += units_by_task.at(node.spec.task_id);
                                        if (on_progress) {
                                          on_progress(done, run.total_units);
                                        }
                                      },
                                  .inputs = run.inputs};
  std::vector<svp::exec::CommittedResult> results =
      run_releasing(request, setup, registry, artifacts);
  report_placement(setup, task_type, results, item_labels(run));
  return results;
}

}  // namespace

svp::audio::AsrChunkDispatch single_asr_chunk_dispatch(const AudioWorkDispatch* dispatch) {
  if (dispatch == nullptr || !dispatch->asr_chunks) {
    return {};
  }
  return [dispatch](const svp::audio::AsrChunkWork& work, const AudioProgress& on_progress)
             -> std::optional<std::vector<std::optional<svp::audio::AsrChunkOutcome>>> {
    auto results = dispatch->asr_chunks({work}, on_progress);
    if (!results) {
      return std::nullopt;
    }
    return std::move(results->front());
  };
}

svp::audio::DiarizationWindowDispatch single_diarization_window_dispatch(
    const AudioWorkDispatch* dispatch) {
  if (dispatch == nullptr || !dispatch->diarization_windows) {
    return {};
  }
  return [dispatch](const svp::audio::DiarizationWindowWork& work,
                    const svp::audio::DiarizationProgressCallback& on_progress)
             -> std::optional<std::vector<std::optional<svp::audio::DiarizationWindowMap>>> {
    auto results = dispatch->diarization_windows({work}, on_progress);
    if (!results) {
      return std::nullopt;
    }
    return std::move(results->front());
  };
}

AudioWorkDispatch make_audio_work_dispatch(std::shared_ptr<const VisionDispatchSetup> setup,
                                           AudioDispatchExtras extras,
                                           const svp::exec::TaskTypeRegistry& registry,
                                           StageOutputAccess& artifacts) {
  AudioWorkDispatch dispatch;

  dispatch.asr_chunks =
      [setup, &registry, &artifacts](const std::vector<svp::audio::AsrChunkWork>& works,
                                     const AudioProgress& on_progress)
      -> std::optional<std::vector<std::vector<std::optional<svp::audio::AsrChunkOutcome>>>> {
    const std::string type(tasks::kAsrChunkBatchTaskType);
    const auto capacity = setup->capacity.find(type);
    if (capacity == setup->capacity.end() || !setup->workers || !setup->workers->takes(type) ||
        works.empty()) {
      return std::nullopt;
    }
    std::optional<PlannedRun> planned = plan_or_skip(*setup, type, [&] {
      PlannedRun run;
      svp::vision::tasks::ItemBatchPolicy policy;
      policy.estimated_seconds_per_item = capacity->second.seconds_per_item;
      for (std::size_t work_index = 0; work_index < works.size(); ++work_index) {
        const svp::audio::AsrChunkWork& work = works[work_index];
        std::vector<std::string> ids{work.model_id, work.vad_model_id};
        if (work.alignment_model_id) {
          ids.push_back(*work.alignment_model_id);
        }
        tasks::AsrChunkBatchTaskInputs inputs;
        inputs.build_session_id = setup->build_session_id;
        inputs.boundary_ordinal = work_index;
        for (const std::string& id : ids) {
          const auto ref = model_ref(*setup, id);
          if (!ref) {
            throw std::runtime_error("model " + id + " was not given to the workers");
          }
          inputs.model_refs.push_back(*ref);
        }
        inputs.audio = audio_input(work.input_wav, artifacts, run.inputs);
        inputs.work = work;
        inputs.batch_policy = policy;
        const std::vector<svp::vision::tasks::ItemBatch> batches =
            svp::vision::tasks::partition_items(work.chunks.size(), policy, [&](std::size_t index) {
              return tasks::asr_chunk_item_bytes(work.chunks[index], index);
            });
        for (const svp::vision::tasks::ItemBatch& batch : batches) {
          run.nodes.push_back({.spec = tasks::make_asr_chunk_batch_task_spec(inputs, batch),
                               .order_key = tasks::asr_chunk_batch_order_key(work_index, batch)});
          run.work_of_node.push_back(work_index);
          run.first_of_node.push_back(batch.first);
          run.items_of_node.push_back(batch.count);
          run.units_of_node.push_back(batch.count);
          run.total_units += batch.count;
        }
      }
      return run;
    });
    if (!planned || planned->nodes.empty()) {
      return std::nullopt;
    }
    std::vector<svp::exec::CommittedResult> results =
        run_planned(*planned, type, on_progress, *setup, registry, artifacts);
    std::vector<std::vector<std::optional<svp::audio::AsrChunkOutcome>>> outcomes(works.size());
    for (std::size_t index = 0; index < works.size(); ++index) {
      outcomes[index].resize(works[index].chunks.size());
    }
    for (std::size_t index = 0; index < results.size(); ++index) {
      std::vector<std::optional<svp::audio::AsrChunkOutcome>> part =
          tasks::read_asr_chunk_batch_output(planned->nodes[index].spec,
                                             results[index].result.outputs,
                                             results[index].payloads);
      std::vector<svp::exec::FramePayload>().swap(results[index].payloads);
      const std::size_t work = planned->work_of_node[index];
      const std::size_t first = planned->first_of_node[index];
      for (std::size_t offset = 0; offset < part.size(); ++offset) {
        outcomes[work][first + offset] = std::move(part[offset]);
      }
    }
    return outcomes;
  };

  // diarize.window is measured on this Mac in its stage (sherpa-onnx is
  // loaded only then); the measurement is taken once per build.
  struct InStageCapacity {
    std::mutex mutex;
    bool measured = false;
    std::optional<DispatchedTypeCapacity> capacity;
  };
  auto in_stage = std::make_shared<InStageCapacity>();
  dispatch.diarization_windows =
      [setup, extras, in_stage, &registry, &artifacts](
          const std::vector<svp::audio::DiarizationWindowWork>& works,
          const AudioProgress& on_progress)
      -> std::optional<std::vector<std::vector<std::optional<svp::audio::DiarizationWindowMap>>>> {
    const std::string type(tasks::kDiarizeWindowTaskType);
    if (!setup->workers || !setup->workers->takes(type) || works.empty()) {
      return std::nullopt;
    }
    std::optional<DispatchedTypeCapacity> capacity;
    if (const auto known = setup->capacity.find(type); known != setup->capacity.end()) {
      capacity = known->second;
    } else if (extras.measure_in_stage) {
      const std::scoped_lock lock(in_stage->mutex);
      if (!in_stage->measured) {
        in_stage->capacity = extras.measure_in_stage(type);
        in_stage->measured = true;
      }
      capacity = in_stage->capacity;
    }
    const auto ref = model_ref(*setup, svp::models::kSherpaOnnxDiarizationModelId);
    const std::optional<std::string> library = tasks::loaded_sherpa_library_identity();
    if (!capacity || !ref || !library) {
      return std::nullopt;
    }
    std::optional<PlannedRun> planned = plan_or_skip(*setup, type, [&] {
      PlannedRun run;
      for (std::size_t work_index = 0; work_index < works.size(); ++work_index) {
        const svp::audio::DiarizationWindowWork& work = works[work_index];
        tasks::DiarizeWindowTaskInputs inputs;
        inputs.build_session_id = setup->build_session_id;
        inputs.run_ordinal = work_index;
        inputs.audio = audio_input(work.wav_path, artifacts, run.inputs);
        inputs.model_ref = *ref;
        inputs.work = work;
        inputs.sherpa_library = *library;
        inputs.seconds_per_audio_second = capacity->seconds_per_item;
        for (std::size_t window = 0; window < work.window_count; ++window) {
          run.nodes.push_back({.spec = tasks::make_diarize_window_task_spec(inputs, window),
                               .order_key = tasks::diarize_window_order_key(work_index, window)});
          const std::size_t pieces =
              svp::audio::diarization_window_piece_count(work.sample_count, window);
          run.work_of_node.push_back(work_index);
          run.first_of_node.push_back(window);
          run.items_of_node.push_back(1);
          run.units_of_node.push_back(pieces);
          run.total_units += pieces;
        }
      }
      return run;
    });
    if (!planned || planned->nodes.empty()) {
      return std::nullopt;
    }
    VisionDispatchSetup run_setup = *setup;
    run_setup.capacity[type] = *capacity;
    std::vector<svp::exec::CommittedResult> results =
        run_planned(*planned, type, on_progress, run_setup, registry, artifacts);
    std::vector<std::vector<std::optional<svp::audio::DiarizationWindowMap>>> maps(works.size());
    for (std::size_t index = 0; index < works.size(); ++index) {
      maps[index].resize(works[index].window_count);
    }
    for (std::size_t index = 0; index < results.size(); ++index) {
      maps[planned->work_of_node[index]][planned->first_of_node[index]] =
          tasks::read_diarize_window_output(planned->nodes[index].spec,
                                            results[index].result.outputs,
                                            results[index].payloads);
      std::vector<svp::exec::FramePayload>().swap(results[index].payloads);
    }
    return maps;
  };

  dispatch.asr_chunks =
      guarded(std::string(tasks::kAsrChunkBatchTaskType), std::move(dispatch.asr_chunks));
  dispatch.diarization_windows = guarded(std::string(tasks::kDiarizeWindowTaskType),
                                         std::move(dispatch.diarization_windows));
  return dispatch;
}

}  // namespace svp::builder::engine
