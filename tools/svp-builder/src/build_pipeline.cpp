#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/build_progress.hpp"
#include "svp/builder/build_thread_plan.hpp"
#include "svp/builder/model_cache_preflight.hpp"

#include "audio_stage.hpp"
#include "build_frame_plan.hpp"
#include "build_interrupt.hpp"
#include "build_pipeline_internal.hpp"
#include "engine/build_assembly.hpp"
#include "engine/build_inputs_digest.hpp"
#include "engine/build_journal_session.hpp"
#include "engine/committed_stage_results.hpp"
#include "engine/build_vision_dispatch.hpp"
#include "engine/distributed_vision_work.hpp"
#include "engine/ocr_batch_observer.hpp"
#include "engine/ocr_execution_policy.hpp"
#include "engine/ocr_frame_batch_plan.hpp"
#include "engine/stage_output_access.hpp"
#include "engine/stage_task_environment.hpp"
#include "engine/stage_task_plan.hpp"
#include "engine/stage_task_registry.hpp"
#include "engine/stage_tasks.hpp"
#include "engine/staging_capture_check.hpp"
#include "engine/whole_stage_scheduler_policy.hpp"
#include "runtime_tools_json.hpp"
#include "staging_cleanup.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/whisper_model.hpp"
#include "svp/core/memory_diagnostics.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/clock.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/journal_error.hpp"
#include "svp/exec/journal_result_commit_sink.hpp"
#include "svp/exec/scheduler.hpp"
#include "svp/exec/source_fingerprint.hpp"
#include "svp/exec/task_type_restricted_executor.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/models/cache.hpp"
#include "svp/models/runtime.hpp"
#include "svp/vision/noise_suppression.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_frame_batch_task.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace svp::builder {

namespace {

// The source every build reads, as the journal's source_fingerprint names it.
constexpr const char* kPrimarySourceId = "source_000";

// The journal's source fingerprint. A build may run from a probe JSON whose
// source file is absent (stages that read the source then degrade as they
// always have); such a source is fingerprinted as zero bytes, so a resume
// still refuses a journal once the file appears or changes.
svp::exec::SourceFingerprintRecord fingerprint_build_source(
    const std::filesystem::path& source_path) {
  std::error_code error;
  if (std::filesystem::is_regular_file(source_path, error)) {
    return svp::exec::fingerprint_source(kPrimarySourceId, source_path);
  }
  return svp::exec::SourceFingerprintRecord{
      .source_id = kPrimarySourceId,
      .path = source_path.string(),
      .size_bytes = 0,
      .mtime_ns = std::nullopt,
      .blake3 = svp::exec::blake3_digest(std::string_view{})};
}

bool should_write_builder_foundation_json(
    const BuildPipelineOptions& options,
    const BuildStageExecutionPlan& stage_plan,
    const PackageSkeletonStageResult& package_result) {
  if (options.verbose) {
    return true;
  }
  if (!stage_plan.run_package_skeleton) {
    return true;
  }
  return package_result.json_output_path == options.output_path;
}

void remove_builder_foundation_json(
    const std::filesystem::path& json_output_path) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(json_output_path, ec)) {
    std::filesystem::remove(json_output_path, ec);
  }
}

std::string join_task_ids(const std::vector<svp::exec::CommittedResult>& committed) {
  std::string ids;
  for (const svp::exec::CommittedResult& result : committed) {
    ids += (ids.empty() ? "" : ", ") + result.result.task_id;
  }
  return ids;
}

// Restores what an interrupted build already committed: each stage task's
// staging capture, in graph order, and its states for the tasks still to run;
// each frame-batch task's output for the OCR reducer.
void restore_committed_tasks(const std::vector<engine::PlannedStageTask>& tasks,
                             const svp::exec::TaskGraph& graph,
                             const std::vector<svp::exec::CommittedResult>& committed,
                             const std::filesystem::path& staging_dir,
                             engine::CommittedStageResults& results) {
  std::map<std::string, const engine::PlannedStageTask*> by_id;
  for (const engine::PlannedStageTask& task : tasks) {
    by_id.emplace(std::string(engine::stage_task_id(task.kind)), &task);
  }
  for (const svp::exec::CommittedResult& result : committed) {
    const auto stage = by_id.find(result.result.task_id);
    if (stage == by_id.end()) {
      const svp::exec::TaskSpec& spec = graph.node(graph.find(result.result.task_id).value()).spec;
      engine::record_committed_result(results, spec, result);
      continue;
    }
    const engine::StageTaskProducts products =
        engine::decode_stage_products(result.result.outputs, result.payloads);
    engine::restore_staging_scope(staging_dir, stage->second->scope, products.staging);
    results.record(result.result.task_id, products);
  }
}

// The OCR stage's frame batches, when it splits (ocr_frame_batch_plan.hpp).
// A --distributed build prepares its workers here, once the OCR work is
// known and before any task runs, and sizes batches from this Mac's measured
// per-sample cost. A resumed build keeps the partition its journal recorded.
struct PlannedOcrExecution {
  std::optional<engine::OcrFrameBatchPlan> batches;
  std::size_t coordinator_slots = engine::kLocalOnlyOcrBatchSlots;
  std::vector<svp::exec::Executor*> workers;
  // --distributed only: the vision stage work this build dispatches (M4) and
  // the models its workers were given.
  std::map<std::string, DispatchedTypeCapacity, std::less<>> dispatched_capacity;
  std::shared_ptr<DispatchedWorkerExecutors> dispatched_workers;
  std::vector<svp::exec::TaskModelRef> dispatched_model_refs;
};

PlannedOcrExecution plan_ocr_execution(
    const BuildPipelineOptions& options, const BuildStageExecutionPlan& stage_plan,
    const svp::media::MediaIngestPlan& plan, const nlohmann::json& plan_json,
    const svp::models::ThreadPlan& thread_plan, bool model_runtime_available,
    const svp::exec::SourceFingerprintRecord& source,
    const std::vector<engine::PlannedStageTask>& tasks, const std::string& build_session_id,
    const engine::BuildJournalSession& journal_session,
    const svp::exec::CancellationToken& cancellation) {
  PlannedOcrExecution execution;
  std::optional<engine::OcrWorkPlan> work = engine::plan_ocr_work({
      .options = options,
      .stage_plan = stage_plan,
      .media_plan = plan,
      .media_plan_json = plan_json,
      .thread_plan = thread_plan,
      .model_runtime_available = model_runtime_available,
      .source_blake3 = source.blake3,
      .source_bytes = source.size_bytes,
  });
  if (!work) {
    return execution;
  }
  svp::vision::OcrBatchPolicy batch_policy;
  if (options.distributed) {
    const DistributedVisionWork vision =
        engine::plan_distributed_vision_work(options.model_cache_dir, thread_plan);
    const DistributedFleet fleet = options.distributed->prepare(DistributedOcrWork{
        .build_session_id = build_session_id,
        .source = work->source,
        .source_path = work->source_path,
        .model_refs = work->model_refs,
        .pp_ocr = work->pp_ocr,
        .model_cache_root = options.model_cache_dir,
        .ffmpeg_path = options.ffmpeg_path,
        .ffmpeg_build = work->ffmpeg_build,
        .thread_plan = thread_plan,
        .cancellation = &cancellation,
        .vision = vision,
    });
    execution.workers = fleet.workers;
    execution.coordinator_slots = std::max<std::size_t>(1, fleet.coordinator_ocr_slots);
    if (fleet.seconds_per_sample) {
      batch_policy.estimated_seconds_per_sample = *fleet.seconds_per_sample;
    }
    execution.dispatched_capacity = fleet.dispatched_capacity;
    execution.dispatched_workers = fleet.dispatched_workers;
    execution.dispatched_model_refs = work->model_refs;
    for (const std::optional<DistributedOnnxWork>* onnx :
         {&vision.text_embeddings, &vision.keyframe_embeddings, &vision.depth}) {
      if (*onnx) {
        execution.dispatched_model_refs.push_back((*onnx)->model_ref);
      }
    }
  }
  const std::vector<std::string> depends_on = engine::ocr_stage_dependencies(tasks);
  if (options.journal_mode == RecoveryJournalMode::resume) {
    if (std::optional<std::vector<svp::vision::OcrSampleBatch>> recorded =
            engine::ocr_batches_from_task_ids(journal_session.recorded_task_ids(),
                                              work->samples.samples.size())) {
      execution.batches = engine::make_ocr_frame_batch_plan_from_batches(
          std::move(*work), std::move(*recorded), batch_policy, build_session_id, depends_on);
      return execution;
    }
  }
  execution.batches = engine::make_ocr_frame_batch_plan(std::move(*work), batch_policy,
                                                         build_session_id, depends_on);
  return execution;
}

void report_resume(const engine::StartedJournal& started, std::size_t task_count,
                   const std::filesystem::path& journal_root, bool quiet) {
  if (quiet || !started.resume_report) {
    return;
  }
  std::cerr << "svp-builder: resuming from " << journal_root.string() << ": "
            << started.committed.size() << " of " << task_count
            << " tasks restored from the journal";
  if (!started.committed.empty()) {
    std::cerr << " (" << join_task_ids(started.committed) << ")";
  }
  std::cerr << "\n";
  for (const svp::exec::DemotedTask& demoted : started.resume_report->demoted) {
    std::cerr << "svp-builder: task " << demoted.task_id
              << " will run again: its journaled output failed verification ("
              << svp::exec::demotion_reason_name(demoted.reason) << ")\n";
  }
}

int cancelled_exit_code() {
  constexpr int kSignalExitStatusBase = 128;
  const int signal_number = build_interrupt_signal();
  return signal_number == 0 ? kBuildFailedExitCode : kSignalExitStatusBase + signal_number;
}

}  // namespace

BuildPipelineResult BuildPipeline::run(const BuildPipelineOptions& options) const {
  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }
  std::optional<svp::models::ThreadPlanResolution> resolved_thread_plan;
  const auto with_plan = [&resolved_thread_plan](BuildPipelineResult result) {
    result.thread_plan = resolved_thread_plan;
    return result;
  };
  // Ctrl-C / SIGTERM cancel this build from here on (build_interrupt.hpp).
  svp::exec::CancellationToken cancellation;
  const BuildInterruptScope interrupt_scope(cancellation);
  const auto cancelled_result = [&with_plan]() {
    return with_plan({.exit_code = cancelled_exit_code(),
                      .failure = BuildPipelineFailure::cancelled,
                      .error_message = "build interrupted"});
  };

  try {
    std::ostringstream diag_name;
    diag_name << "svp-builder-memory";
#if defined(__APPLE__)
    diag_name << "-" << static_cast<long long>(getpid());
#endif
    diag_name << ".jsonl";
    svp::core::configure_memory_diagnostics_from_environment(
        std::filesystem::current_path() / "build" / "diagnostics" /
        diag_name.str());
    svp::core::check_memory_limit("builder.run.start", {
        {"source", options.source_path},
        {"output", options.output_path.string()},
        {"staging_dir", options.staging_dir.string()}
    });

    BuildPipelineOptions effective_options = options;
    if (effective_options.model_cache_dir.empty()) {
      effective_options.model_cache_dir = svp::models::model_cache_root();
    }

    const std::string stop_after_name(build_stage_name(effective_options.stop_after));
    const BuildStageExecutionPlan stage_plan =
        execution_plan_for_stage(effective_options.stop_after);

    if (schedules_model_backed_work(stage_plan)) {
      verify_authoritative_model_cache(effective_options.model_cache_dir);
    }

    // Every runtime thread count for this build is fixed here, once, and
    // passed to each stage. Memory diagnostics must be configured first: they
    // gate the recognition-worker override.
    resolved_thread_plan = resolve_build_thread_plan(
        effective_options, svp::models::detect_host_cpu_topology(),
        svp::models::process_environment_lookup(),
        svp::core::memory_diagnostics_enabled());
    const svp::models::ThreadPlan& thread_plan = resolved_thread_plan->plan;

    sink->emit(make_stage_started(ProgressStageId::media_probe));
    const svp::media::MediaIngestPlan plan =
        svp::media::build_media_ingest_plan(
            options.source_path,
            load_or_run_probe(options.source_path, options.probe_json_path,
                              options.ffprobe_path));
    const nlohmann::json plan_json = svp::media::media_ingest_plan_to_json(plan);
    sink->emit(make_stage_completed(ProgressStageId::media_probe));

    const bool user_supplied_staging = !effective_options.staging_dir.empty();
    const std::filesystem::path staging_dir =
        user_supplied_staging
            ? effective_options.staging_dir
            : default_staging_dir_for_output(effective_options.output_path);
    // A resumed build rebuilds staging from the journal alone, so whatever an
    // interrupted run left in a reused staging directory is cleared first.
    if (effective_options.reset_staging_before_stages ||
        (user_supplied_staging &&
         effective_options.journal_mode == RecoveryJournalMode::resume)) {
      std::filesystem::remove_all(staging_dir);
    }
    std::filesystem::create_directories(staging_dir);
    StagingCleanupGuard staging_guard(staging_dir, user_supplied_staging);
    const bool model_runtime_available = svp::models::OnnxSession::is_available();
    svp::models::set_onnx_verbose(options.verbose);
    svp::audio::set_whisper_verbose(options.verbose);
    svp::vision::set_opencv_verbose(options.verbose);

    if (!effective_options.sherpa_lib_path.empty()) {
      svp::audio::set_sherpa_lib_path(effective_options.sherpa_lib_path);
    }

    // Frame IDs come from the plan, not from which stage decodes first.
    svp::vision::FrameCatalog planned_catalog;
    plan_build_frames(planned_catalog, stage_plan, plan,
                      effective_options.visual_tracking_quality,
                      model_runtime_available);

    // Plan the build as whole-stage tasks.
    const BuilderConcurrencyPolicy policy =
        builder_concurrency_policy(effective_options.performance, 1);
    const std::vector<engine::PlannedStageTask> tasks = engine::plan_stage_tasks({
        .stage_plan = stage_plan,
        .serial_pipeline = effective_options.serial_pipeline,
        .single_video_heavy_lanes = policy.single_video_heavy_lanes,
        .microphone_stream_mode =
            stage_plan.run_audio &&
            audio_uses_microphone_path(effective_options, plan, model_runtime_available),
        .svpi_publication = stage_plan.run_package_skeleton && effective_options.svpi.has_value(),
    });
    const svp::exec::SourceFingerprintRecord source =
        fingerprint_build_source(std::filesystem::path(options.source_path));
    const std::string build_inputs = engine::build_inputs_blake3(
        engine::describe_build_inputs(effective_options, source, plan_json, thread_plan));

    engine::BuildJournalSession journal_session(
        effective_options.journal_mode,
        effective_options.journal_output_path.empty() ? effective_options.output_path
                                                      : effective_options.journal_output_path);
    const std::string build_session_id = journal_session.prepare();
    PlannedOcrExecution ocr_execution = plan_ocr_execution(
        effective_options, stage_plan, plan, plan_json, thread_plan, model_runtime_available,
        source, tasks, build_session_id, journal_session, cancellation);
    const engine::OcrFrameBatchPlan* ocr_batches =
        ocr_execution.batches ? &*ocr_execution.batches : nullptr;
    const svp::exec::TaskGraph graph =
        engine::make_build_task_graph(tasks, build_session_id, build_inputs, ocr_batches);
    std::optional<engine::StartedJournal> started_journal;
    try {
      started_journal.emplace(journal_session.start(graph, source));
    } catch (const svp::exec::JournalError& error) {
      if (error.code() != svp::exec::JournalErrorCode::io_error ||
          effective_options.journal_mode == RecoveryJournalMode::resume) {
        throw;
      }
      // The journal lives beside the output; if it cannot be created there,
      // neither can the output.
      const std::string message =
          "failed to write SVP package: " +
          resolve_package_skeleton_output_paths(effective_options.output_path)
              .package_path.string() +
          " (cannot create its recovery journal: " + error.what() + ")";
      std::cerr << "svp-builder: " << message << "\n";
      return with_plan({.exit_code = kBuildFailedExitCode,
                        .failure = BuildPipelineFailure::package_write,
                        .error_message = message});
    }
    engine::StartedJournal& started = *started_journal;
    if (cancellation.requested()) {
      started.journal.set_build_session_status(build_session_id,
                                               svp::exec::BuildSessionStatus::interrupted);
      return cancelled_result();
    }

    engine::CommittedStageResults results;
    restore_committed_tasks(tasks, graph, started.committed, staging_dir, results);
    report_resume(started, graph.size(), journal_session.journal_root(),
                  effective_options.quiet);

    svp::exec::TaskTypeRegistry registry;
    engine::StageOutputAccess outputs;
    engine::StageExitRecord stage_exits;
    const auto pp_ocr_sessions = std::make_shared<svp::vision::tasks::PpOcrSessionPool>();
    // --distributed: the vision stages hand their per-item work to tasks on
    // this Mac's measured slots and the workers (M4, vision_dispatch_setup
    // .hpp). Other builds have no dispatch, and their stages run as always.
    std::unique_ptr<engine::BuildVisionDispatch> vision_dispatch;
    if (ocr_batches != nullptr && !ocr_execution.dispatched_capacity.empty()) {
      vision_dispatch = engine::make_build_vision_dispatch({
          .source_path = ocr_batches->work.source_path,
          .registry = registry,
          .model_cache_root = effective_options.model_cache_dir,
          .ffmpeg_path = effective_options.ffmpeg_path,
          .pp_ocr_sessions = pp_ocr_sessions,
          .setup = {.build_session_id = build_session_id,
                    .source = ocr_batches->work.source,
                    .ffmpeg_build = ocr_batches->work.ffmpeg_build,
                    .model_refs = ocr_execution.dispatched_model_refs,
                    .capacity = ocr_execution.dispatched_capacity,
                    .workers = ocr_execution.dispatched_workers,
                    .cancellation = &cancellation,
                    .report = !effective_options.quiet,
                    .release_idle_models = {}}});
    }
    const engine::StageTaskEnvironment environment{
        .options = effective_options,
        .stage_plan = stage_plan,
        .plan = plan,
        .plan_json = plan_json,
        .staging_dir = staging_dir,
        .thread_plan = thread_plan,
        .model_runtime_available = model_runtime_available,
        .planned_catalog = planned_catalog,
        .progress_sink = *sink,
        .results = results,
        .ocr_batches = ocr_batches,
        .pp_ocr_sessions = pp_ocr_sessions,
        .vision_dispatch = vision_dispatch ? &vision_dispatch->dispatch : nullptr};
    engine::register_stage_task_types(registry, tasks, environment, outputs, stage_exits);
    if (ocr_batches != nullptr) {
      outputs.register_input(ocr_batches->work.source, ocr_batches->work.source_path);
      svp::vision::tasks::register_ocr_frame_batch_task(
          registry,
          svp::vision::tasks::OcrFrameBatchWorkerEnvironment{
              .model_cache_root = effective_options.model_cache_dir,
              .ffmpeg_path = effective_options.ffmpeg_path,
              .write_output =
                  [&outputs](std::span<const std::byte> bytes, std::string media_type,
                             std::string role) {
                    return outputs.put(bytes, std::move(media_type), std::move(role));
                  },
              .model_cache_for = {},
              .record_start_failures = true},
          environment.pp_ocr_sessions);
    }

    // One slot per stage task that can run at once: the graph's edges encode
    // the concurrency policy, so they alone bound concurrency. Frame batches
    // have executors of their own: this Mac's OCR slots and, when
    // distributed, the workers'.
    svp::exec::InProcessExecutor executor(
        registry, outputs,
        svp::exec::InProcessExecutorOptions{
            .executor_id = "in-process",
            .threads = engine::stage_task_graph_width(tasks),
            .worker_session_id = "ws_" + build_session_id,
            .runtime_id = {}});
    const std::set<std::string, std::less<>> frame_batch_types{
        std::string(svp::vision::tasks::kOcrFrameBatchTaskType)};
    svp::exec::TaskTypeExcludingExecutor stages_only(executor, frame_batch_types);
    std::optional<svp::exec::InProcessExecutor> ocr_executor;
    std::optional<svp::exec::TaskTypeRestrictedExecutor> ocr_only;
    std::vector<svp::exec::Executor*> executors{&stages_only};
    if (ocr_batches != nullptr) {
      ocr_executor.emplace(registry, outputs,
                           svp::exec::InProcessExecutorOptions{
                               .executor_id = "in-process-ocr",
                               .threads = ocr_execution.coordinator_slots,
                               .worker_session_id = "ws_" + build_session_id + "_ocr",
                               .runtime_id = {}});
      ocr_only.emplace(*ocr_executor, frame_batch_types);
      executors.push_back(&*ocr_only);
      executors.insert(executors.end(), ocr_execution.workers.begin(),
                       ocr_execution.workers.end());
    }
    svp::exec::JournalResultCommitSink journal_sink(started.journal);
    engine::StageResultCommitSink commit_sink(journal_sink, results);
    const svp::exec::SteadyClock clock;
    svp::exec::SchedulerPolicy scheduler_policy = engine::whole_stage_scheduler_policy();
    scheduler_policy.task_types.emplace(std::string(svp::vision::tasks::kOcrFrameBatchTaskType),
                                        engine::ocr_frame_batch_task_policy());
    std::optional<engine::OcrBatchObserver> ocr_observer;
    svp::exec::AttemptObserver observer;
    if (ocr_batches != nullptr) {
      std::set<std::string> resumed_ids;
      for (const svp::exec::CommittedResult& committed : started.committed) {
        resumed_ids.insert(committed.result.task_id);
      }
      ocr_observer.emplace(*ocr_batches, *sink, clock, effective_options.quiet, resumed_ids);
      observer = [&ocr_observer](const svp::exec::AttemptEvent& event) {
        ocr_observer->observe(event);
      };
    }
    const svp::exec::BuildOutcome outcome =
        svp::exec::Scheduler(scheduler_policy, clock)
            .run(graph, executors, commit_sink, cancellation, observer, started.committed);
    if (ocr_observer && effective_options.distributed && !effective_options.quiet) {
      std::cerr << ocr_observer->summary();
    }

    if (outcome.status == svp::exec::BuildStatus::cancelled) {
      started.journal.set_build_session_status(build_session_id,
                                               svp::exec::BuildSessionStatus::interrupted);
      started.journal.close();
      return cancelled_result();
    }
    if (outcome.status == svp::exec::BuildStatus::failed) {
      started.journal.set_build_session_status(build_session_id,
                                               svp::exec::BuildSessionStatus::failed);
      started.journal.close();
      if (const std::optional<int> exit_code = stage_exits.exit_code()) {
        return with_plan({.exit_code = *exit_code});
      }
      const std::string message =
          outcome.failure ? outcome.failure->message : std::string("build failed");
      svp::core::trace_memory_event("builder.run.exception", {{"error", message}});
      std::cerr << "svp-builder: " << message << "\n";
      return with_plan({.exit_code = kBuildFailedExitCode,
                        .failure = BuildPipelineFailure::processing,
                        .error_message = message});
    }

    if (const std::optional<std::string> problem =
            engine::published_output_problem(tasks, results, effective_options)) {
      throw engine::RecoveryJournalBlocked(
          "--resume: " + *problem + "; pass --fresh to rebuild it");
    }
    for (const std::string& problem :
         engine::unreproducible_staging_entries(staging_dir, tasks, results)) {
      std::cerr << "svp-builder: warning: the recovery journal would not reproduce "
                   "staging entry "
                << problem << "\n";
    }

    nlohmann::json output = engine::assemble_foundation_json(tasks, results);
    PackageSkeletonStageResult package_result;
    package_result.json_output_path = options.output_path;
    if (stage_plan.run_package_skeleton) {
      package_result = engine::committed_package_result(results, effective_options);
    }
    BuildPipelineContext context{effective_options, stage_plan, plan, staging_dir,
                                 thread_plan, model_runtime_available, output,
                                 planned_catalog, *sink};

    output["builder_command"] = {
        {"command", "build"},
        {"stop_after", stop_after_name},
        {"ocr_performance", effective_options.performance.ocr_performance_profile},
        {"visual_tracking_quality", effective_options.visual_tracking_quality},
        {"valid_svp_package_written", package_result.validator_passes},
        {"thread_plan",
         svp::models::thread_plan_resolution_to_json(*resolved_thread_plan)},
    };
    if (effective_options.runtime_tools) {
      output["builder_command"]["runtime_tools"] =
          runtime_tools_json(*effective_options.runtime_tools);
    }

    if (stage_plan.run_package_skeleton) {
      output["builder_command"]["package_path"] =
          package_result.package_path.string();
      output["builder_command"]["validator"] = {
          {"exit_code", package_result.validator_exit_code},
          {"validation_report_stored", package_result.validation_report_stored},
          {"validator_proven_valid", package_result.validator_passes},
          {"report", package_result.validation_report_json}};
      if (package_result.validation_report_stored) {
        output["builder_command"]["validator"]["validation_report_path"] =
            "provenance/validation.json";
      }
    }

    const bool write_foundation_json =
        should_write_builder_foundation_json(options, stage_plan, package_result);
    if (write_foundation_json) {
      write_json_file(package_result.json_output_path, output);
      emit_artifact_written(context,
                            stage_plan.run_package_skeleton
                                ? ProgressStageId::package_write
                                : ProgressStageId::media_probe,
                            package_result.json_output_path,
                            "builder foundation JSON");
    } else {
      remove_builder_foundation_json(package_result.json_output_path);
    }

    if (options.verbose) {
      print_build_progress(context, package_result);
    }

    BuildPipelineResult finished;
    if (stage_plan.run_package_skeleton && effective_options.svpi) {
      finished.svpi = engine::svpi_publication_result(results);
    }

    // RC2 §20.5.1: the journal is deleted only after the published artifact
    // was finalized and passed strict validation.
    const std::optional<std::string> unpublished =
        engine::unfinished_publication(stage_plan, package_result, finished.svpi);
    std::string journal_note;
    if (!unpublished) {
      started.journal.finish_success(svp::exec::JournalRetention::delete_on_success);
    } else {
      started.journal.set_build_session_status(build_session_id,
                                               svp::exec::BuildSessionStatus::failed);
      started.journal.close();
      journal_note = engine::kept_journal_note(*unpublished, journal_session.journal_root());
      std::cerr << "svp-builder: " << journal_note << "\n";
    }
    if (stage_plan.run_package_skeleton && !package_result.package_written) {
      // A package that was never written is a failed build, whatever the
      // earlier stages reported; never let it fall through to success.
      const std::string message =
          "failed to write SVP package: " + package_result.package_path.string();
      std::cerr << "svp-builder: " << message << "\n";
      finished.exit_code = kBuildFailedExitCode;
      finished.failure = BuildPipelineFailure::package_write;
      finished.error_message = message + "; " + journal_note;
      return with_plan(std::move(finished));
    }
    if (stage_plan.run_package_skeleton && !package_result.validator_passes) {
      svp::core::check_memory_limit("builder.run.complete.validator_failed");
      finished.exit_code = package_result.validator_exit_code;
      finished.error_message = journal_note;
      return with_plan(std::move(finished));
    }
    svp::core::check_memory_limit("builder.run.complete");
    finished.error_message = journal_note;
    staging_guard.cleanup_on_success();
    return with_plan(std::move(finished));
  } catch (const DistributedPreparationError& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return with_plan({.exit_code = kBuildFailedExitCode,
                      .failure = BuildPipelineFailure::processing,
                      .error_message = error.what()});
  } catch (const engine::RecoveryJournalBlocked& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
    return with_plan({.exit_code = kBuildFailedExitCode,
                      .failure = BuildPipelineFailure::recovery_journal,
                      .error_message = error.what()});
  } catch (const ModelCachePreflightError& error) {
    svp::core::trace_memory_event("builder.run.exception", {
        {"error", error.what()}
    });
    std::cerr << "svp-builder: " << error.what() << "\n";
    return with_plan({.exit_code = kBuildFailedExitCode,
            .failure = BuildPipelineFailure::model_cache_preflight,
            .error_message = error.what()});
  } catch (const std::exception& error) {
    svp::core::trace_memory_event("builder.run.exception", {
        {"error", error.what()}
    });
    std::cerr << "svp-builder: " << error.what() << "\n";
    return with_plan({.exit_code = kBuildFailedExitCode,
            .failure = BuildPipelineFailure::processing,
            .error_message = error.what()});
  }
}

}  // namespace svp::builder
