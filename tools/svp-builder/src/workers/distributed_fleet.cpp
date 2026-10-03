#include "distributed_fleet.hpp"

#include "audio_calibration_runs.hpp"
#include "build_blob_release.hpp"
#include "coordinator_context.hpp"
#include "dispatched_calibration_runs.hpp"
#include "fleet_pairing.hpp"
#include "track_window_calibration_runs.hpp"
#include "worker_reach.hpp"

#include "calibration/audio_capacity_workloads.hpp"
#include "calibration/ocr_capacity_calibration.hpp"
#include "svp/audio/tasks/diarize_window.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/local_load.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <iostream>
#include <sstream>
#include <thread>

namespace svp::builder::workers {
namespace {

using namespace svp::exec::worker;
namespace remote = svp::exec::remote;

struct WorkerOutcome {
  CoordinatorPairingRecord record;
  bool ready = false;
  std::string problem;
  std::size_t slots = 0;
  // Measured slots per dispatched vision task type (types it could not
  // measure are missing: it runs none of them).
  std::map<std::string, std::size_t, std::less<>> dispatched_slots;
  std::size_t tracking_slots = 0;
  // Which kinds of task this worker takes: one it could not calibrate for
  // does not cost it the others.
  bool ocr_ready = false;
  bool tracking_ready = false;
  std::string detail;
};

// The tracking calibration setup and clip when the build splits tracking.
struct TrackingCalibration {
  TrackWindowCalibrationSetup setup;
  std::unique_ptr<CalibrationClipFile> clip;
};

// The audio work's calibration setup and speech clip (M5).
struct AudioCalibration {
  calibration::AudioCalibrationSetup setup;
  std::shared_ptr<CalibrationClipFile> clip;
};

// The dispatched vision work's calibration setup: the OCR work's PP-OCR, the
// vision models, and the decoder.
calibration::DispatchedCalibrationSetup dispatched_setup(const DistributedOcrWork& work) {
  return calibration::DispatchedCalibrationSetup{.pp_ocr = work.pp_ocr,
                                                 .pp_ocr_model_refs = work.model_refs,
                                                 .vision = work.vision,
                                                 .ffmpeg_build = work.ffmpeg_build};
}

// Fresh executors per dispatched stage run (DispatchedWorkerExecutors): one
// RemoteExecutor per worker that measured the type, with its slots for it.
// Each run opens its own sessions, so a stage's tasks never wait behind
// another run's leases, and the worker admits each lease by memory.
class FleetDispatchedExecutors final : public DispatchedWorkerExecutors {
 public:
  struct Worker {
    svp::exec::remote::PairingKey key;
    std::map<std::string, std::size_t, std::less<>> slots;
  };

  FleetDispatchedExecutors(std::vector<Worker> workers,
                           std::shared_ptr<const WorkerSupplies> supplies,
                           std::shared_ptr<BuildBlobRelease> release)
      : workers_(std::move(workers)),
        supplies_(std::move(supplies)),
        release_(std::move(release)) {}

  std::vector<std::unique_ptr<svp::exec::Executor>> make(std::string_view task_type) override {
    return make(task_type, {});
  }

  // Sessions that run tasks reading files made after prepare() (the staged
  // analysis audio) are supplied those too, each sent only when the worker
  // lacks it.
  std::vector<std::unique_ptr<svp::exec::Executor>> make(
      std::string_view task_type, const std::vector<DispatchedInput>& inputs) override {
    std::shared_ptr<const WorkerSupplies> supplies = supplies_;
    if (!inputs.empty()) {
      auto with_inputs = std::make_shared<WorkerSupplies>(*supplies_);
      for (const DispatchedInput& input : inputs) {
        with_inputs->blobs.push_back(BlobSource{
            .ref = BlobRef{.blake3 = input.ref.blake3, .bytes = input.ref.bytes},
            .file = input.file});
        // Made for this build only: released with its source.
        release_->add_blob(with_inputs->blobs.back().ref);
      }
      supplies = std::move(with_inputs);
    }
    std::vector<std::unique_ptr<svp::exec::Executor>> executors;
    for (const Worker& worker : workers_) {
      if (!takes(worker, task_type)) {
        continue;
      }
      const std::size_t slots = worker.slots.find(task_type)->second;
      executors.push_back(std::make_unique<svp::exec::remote::RemoteExecutor>(
          svp::exec::remote::RemoteExecutorOptions{
              .executor_id = "worker." + worker.key.pairing_id,
              .connector = {.pairing = worker.key},
              .slots = slots,
              .session_preamble =
                  make_supplying_preamble(declaring_capacity(supplies, task_type, slots))}));
    }
    return executors;
  }

  bool takes(std::string_view task_type) const override {
    return std::any_of(workers_.begin(), workers_.end(),
                       [&](const Worker& worker) { return takes(worker, task_type); });
  }

 private:
  static bool takes(const Worker& worker, std::string_view task_type) {
    const auto slots = worker.slots.find(task_type);
    return slots != worker.slots.end() && slots->second > 0;
  }

  std::vector<Worker> workers_;
  std::shared_ptr<const WorkerSupplies> supplies_;
  std::shared_ptr<BuildBlobRelease> release_;
};

std::string seconds_since(std::chrono::steady_clock::time_point start) {
  std::ostringstream out;
  out.precision(1);
  out << std::fixed
      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s";
  return out.str();
}

// Like a RemoteExecutor session (remote_executor.hpp), keeps trying for the
// reconnect window: a worker whose job launchd just (re)started can be
// advertised before it accepts connections. Authentication failures end at
// once; waiting does not change who holds the secret.
std::unique_ptr<WorkerConnection> connect_within_window(
    const remote::PairingKey& key, const svp::exec::CancellationToken& cancellation) {
  const auto give_up =
      std::chrono::steady_clock::now() + remote::kDefaultReconnectWindow;
  while (true) {
    try {
      return connect_to_worker(key);
    } catch (const remote::RemoteTransportError& error) {
      if (error.code() == remote::RemoteErrorCode::authentication_failed ||
          cancellation.requested() ||
          std::chrono::steady_clock::now() + remote::kDefaultReconnectPause >= give_up) {
        throw;
      }
    }
    std::this_thread::sleep_for(remote::kDefaultReconnectPause);
    if (cancellation.requested()) {
      throw std::runtime_error("build cancelled");
    }
  }
}

WorkerOutcome prepare_worker(CoordinatorPairingRecord record, const WorkerSupplies& supplies,
                             const OcrCalibrationSetup& setup,
                             const calibration::DispatchedCalibrationSetup& dispatched,
                             const TrackingCalibration* tracking, const AudioCalibration& audio,
                             CalibrationClipFile& clip, const CalibrationStore& store,
                             const svp::exec::CancellationToken& cancellation,
                             BuildBlobRelease& release) {
  WorkerOutcome outcome{.record = std::move(record)};
  const auto start = std::chrono::steady_clock::now();
  try {
    WorkerHelloAck ack;
    TransferStats stats;
    std::string route;
    {
      const std::unique_ptr<WorkerConnection> connection =
          connect_within_window(outcome.record.key, cancellation);
      route = connection->connection.route.route.interface_name + " (" +
              std::string(remote::route_medium_name(connection->connection.route.route.medium)) +
              ")";
      WorkerSessionClient client(*connection->reader, *connection->writer);
      // A worker that accepted HELLO may hold this build's source, also
      // when a transfer failed part way; one that refused holds nothing.
      SuppliedSession session;
      try {
        session = supply_worker_session(*connection->reader, *connection->writer, supplies);
      } catch (const WorkerError& error) {
        if (error.code() != WorkerErrorCode::refused) {
          release.add_worker(outcome.record);
        }
        throw;
      } catch (...) {
        release.add_worker(outcome.record);
        throw;
      }
      release.add_worker(outcome.record);
      ack = session.ack;
      stats = session.stats;
      client.shutdown();
    }
    std::ostringstream detail;
    detail << "via " << route << ", sent " << format_bytes(stats.bytes_sent) << " ("
           << (stats.runtime_pushed ? "runtime, " : "") << stats.model_bundles_pushed.size()
           << " model bundle(s), " << stats.blobs_sent << " blob(s))";
    std::string problems;
    try {
      const CalibrationOutcome calibration = ensure_worker_calibration(
          outcome.record.key, supplies, ack, setup, clip, store, cancellation);
      outcome.slots = calibration.ocr.slots;
      outcome.ocr_ready = true;
      detail << ", " << (calibration.measured ? "calibrated now: " : "calibration: ")
             << describe_calibration(calibration.ocr);
    } catch (const std::exception& error) {
      problems = std::string("OCR calibration: ") + error.what();
      detail << ", takes no OCR (calibration failed: " << error.what() << ")";
    }
    // A type this worker cannot measure is left to the other executors.
    for (const std::string& type : calibration::dispatched_task_types(dispatched.vision)) {
      try {
        const CapacityOutcome capacity =
            ensure_worker_capacity(outcome.record.key, supplies, ack, type, dispatched, clip,
                                   setup.ffmpeg_path, store, cancellation);
        outcome.dispatched_slots[type] = capacity.capacity.slots;
        detail << "; " << (capacity.measured ? "calibrated now: " : "")
               << describe_capacity(type, capacity.capacity);
      } catch (const std::exception& error) {
        detail << "; " << type << " not used: " << error.what();
      }
    }
    // The audio types (M5), on the speech clip.
    for (const std::string& type : calibration::audio_task_types(audio.setup.audio)) {
      try {
        const CapacityOutcome capacity =
            ensure_worker_audio_capacity(outcome.record.key, supplies, ack, type, audio.setup,
                                         *audio.clip, store, cancellation);
        outcome.dispatched_slots[type] = capacity.capacity.slots;
        detail << "; " << (capacity.measured ? "calibrated now: " : "")
               << describe_capacity(type, capacity.capacity);
      } catch (const std::exception& error) {
        detail << "; " << type << " not used: " << error.what();
      }
    }
    if (tracking != nullptr) {
      try {
        const CapacityOutcome capacity = ensure_worker_track_window_calibration(
            outcome.record.key, supplies, ack, tracking->setup, *tracking->clip, cancellation);
        outcome.tracking_slots = capacity.capacity.slots;
        outcome.tracking_ready = true;
        detail << "; " << (capacity.measured ? "tracking calibrated now: " : "tracking: ")
               << describe_track_window_calibration(capacity.capacity);
      } catch (const std::exception& error) {
        problems += std::string(problems.empty() ? "" : "; ") + "tracking calibration: " +
                    error.what();
        detail << "; " << svp::vision::tasks::kTrackWindowTaskType
               << " not used: " << error.what();
      }
    }
    outcome.ready =
        outcome.ocr_ready || outcome.tracking_ready || !outcome.dispatched_slots.empty();
    if (!outcome.ready) {
      outcome.problem = problems;
      return outcome;
    }
    detail << ", ready in " << seconds_since(start);
    outcome.detail = detail.str();
  } catch (const std::exception& error) {
    outcome.problem = error.what();
  }
  return outcome;
}

// This build's in-process tasks, in this user's LocalLoad record (M6,
// local_load.hpp). While it lives this Mac also counts as coordinating a
// video, so the agent here takes no whole-video job from another Mac.
class RecordedLocalLoad final : public LocalTaskLoad {
 public:
  RecordedLocalLoad() { recorder_.add(kCoordinatingTaskType); }
  void started(std::string_view task_type) override { recorder_.add(task_type); }
  void finished(std::string_view task_type) override { recorder_.remove(task_type); }

 private:
  LocalLoadRecorder recorder_;
};

std::string worker_name(const CoordinatorPairingRecord& record) {
  return record.key.pairing_id + " (" +
         (record.worker.ssh_target.empty() ? record.worker.join_id : record.worker.ssh_target) +
         ")";
}

}  // namespace

PairedWorkerFleet::PairedWorkerFleet(DistributedFleetOptions options)
    : options_(std::move(options)), release_(std::make_shared<BuildBlobRelease>()) {}

// The build has ended (succeeded, failed, or cancelled) when its fleet is
// destroyed. Its worker sessions are closed first, so each worker's session
// processes let go of the source before it is released.
PairedWorkerFleet::~PairedWorkerFleet() {
  restricted_.clear();
  remotes_.clear();
  release_->release([quiet = options_.quiet](const std::string& line) {
    if (!quiet) {
      std::cerr << "svp-builder: " << line << "\n";
    }
  });
}

DistributedFleet PairedWorkerFleet::prepare(const DistributedOcrWork& work) {
  const auto log = [this](const std::string& line) {
    if (!options_.quiet) {
      std::cerr << "svp-builder: " << line << "\n";
    }
  };
  const svp::exec::CancellationToken never_cancelled;
  const svp::exec::CancellationToken& cancellation =
      work.cancellation != nullptr ? *work.cancellation : never_cancelled;
  // From now until the build ends this Mac is coordinating (M6).
  const std::shared_ptr<LocalTaskLoad> local_load = std::make_shared<RecordedLocalLoad>();

  // What every worker is sent. If this Mac cannot assemble it (no pairing
  // store, no runtime manifest, a model bundle missing), no worker can be
  // used: the build runs on this Mac alone, unless --require-workers asks
  // for workers it cannot have.
  // A fleet coordinator first pairs the fleet's joinable workers it has not
  // paired yet (fleet_pairing.hpp), so a Mac installed with `worker install
  // --join` takes part in this build. A fleet problem is reported and never
  // keeps the build from the workers already paired.
  const auto pair_fleet_workers = [&](const PairingDirectory& pairings,
                                      const CoordinatorRuntime& runtime) {
    try {
      const std::optional<FleetMembership> membership = load_fleet_membership(default_fleet_dir());
      if (!membership) {
        return;
      }
      std::ostringstream progress;
      const FleetPairingReport report = pair_joinable_fleet_workers(
          *membership, pairings, runtime.runtime_id, runtime.kind, &progress);
      std::istringstream lines(progress.str());
      for (std::string line; std::getline(lines, line);) {
        log("--distributed: " + line);
      }
      for (const std::string& problem : report.problems) {
        log("warning: --distributed: fleet: " + problem);
      }
    } catch (const std::exception& error) {
      log(std::string("warning: --distributed: fleet pairing skipped: ") + error.what());
    }
  };

  std::vector<CoordinatorPairingRecord> records;
  auto supplies = std::make_shared<WorkerSupplies>();
  try {
    const PairingDirectory pairings(default_coordinator_pairings_dir());
    supplies->runtime = locate_coordinator_runtime(current_executable());
    pair_fleet_workers(pairings, supplies->runtime);
    records = load_coordinator_pairings(pairings);
    supplies->hello = make_coordinator_hello(supplies->runtime, work.thread_plan,
                                             model_set_summary(work.model_cache_root));
    std::vector<std::string> model_ids;
    for (const svp::exec::TaskModelRef& ref : work.model_refs) {
      model_ids.push_back(ref.model_id);
    }
    // The tracking windows' detector, depth, and embedding bundles (M4).
    if (work.tracking) {
      for (const svp::exec::TaskModelRef& ref : work.tracking->model_refs) {
        if (std::find(model_ids.begin(), model_ids.end(), ref.model_id) == model_ids.end()) {
          model_ids.push_back(ref.model_id);
        }
      }
    }
    // The dispatched vision work's models (M4).
    for (const std::optional<DistributedOnnxWork>* onnx :
         {&work.vision.text_embeddings, &work.vision.keyframe_embeddings, &work.vision.depth}) {
      if (*onnx && std::find(model_ids.begin(), model_ids.end(), (*onnx)->model_ref.model_id) ==
                       model_ids.end()) {
        model_ids.push_back((*onnx)->model_ref.model_id);
      }
    }
    // The audio work's models (M5).
    std::vector<svp::exec::TaskModelRef> audio_refs = work.audio.asr_model_refs;
    if (work.audio.diarization_model_ref) {
      audio_refs.push_back(*work.audio.diarization_model_ref);
    }
    for (const svp::exec::TaskModelRef& ref : audio_refs) {
      if (std::find(model_ids.begin(), model_ids.end(), ref.model_id) == model_ids.end()) {
        model_ids.push_back(ref.model_id);
      }
    }
    supplies->models = prepare_model_bundles(work.model_cache_root, model_ids);
    supplies->blobs = {BlobSource{.ref = BlobRef{.blake3 = work.source.blake3,
                                                 .bytes = work.source.bytes},
                                  .file = work.source_path}};
    release_->set_hello(supplies->hello);
    release_->add_blob(supplies->blobs.front().ref);
  } catch (const std::exception& error) {
    if (options_.require_workers > 0) {
      throw DistributedPreparationError("--require-workers " +
                                        std::to_string(options_.require_workers) +
                                        ": cannot prepare workers: " + error.what());
    }
    log(std::string("warning: --distributed: cannot prepare workers (") + error.what() +
        "); OCR runs on this Mac only");
    // As a build without measurements runs (kLocalOnlyOcrBatchSlots).
    DistributedFleet alone{.workers = {}, .coordinator_ocr_slots = 1, .seconds_per_sample = {}};
    alone.local_load = local_load;
    return alone;
  }

  const OcrCalibrationSetup setup{.pp_ocr = work.pp_ocr,
                                  .model_refs = work.model_refs,
                                  .ffmpeg_path = work.ffmpeg_path,
                                  .ffmpeg_build = work.ffmpeg_build};
  const calibration::DispatchedCalibrationSetup dispatched = dispatched_setup(work);
  CalibrationClipFile clip(work.ffmpeg_path);
  std::optional<TrackingCalibration> tracking;
  if (work.tracking) {
    tracking.emplace();
    tracking->setup = TrackWindowCalibrationSetup{.options = work.tracking->options,
                                                  .model_refs = work.tracking->model_refs,
                                                  .model_cache_root = work.model_cache_root,
                                                  .ffmpeg_path = work.ffmpeg_path,
                                                  .ffmpeg_build = work.ffmpeg_build};
    tracking->clip = track_window_calibration_clip_file(tracking->setup);
  }
  AudioCalibration audio;
  audio.setup = calibration::AudioCalibrationSetup{.audio = work.audio,
                                                   .thread_plan = work.thread_plan,
                                                   .sherpa_library = {}};
  if (audio.setup.audio.diarization_model_ref) {
    // Named without loading it: sherpa-onnx must not load in this process
    // before the build's own ONNX Runtime models. Workers refuse windows
    // from another library, so without one there are no windows to send.
    if (const std::optional<std::string> library =
            svp::audio::tasks::expected_sherpa_library_identity()) {
      audio.setup.sherpa_library = *library;
    } else {
      audio.setup.audio.diarization_model_ref.reset();
    }
  }
  audio.clip = std::make_shared<CalibrationClipFile>(
      work.ffmpeg_path, [](const std::filesystem::path& ffmpeg, const std::filesystem::path& dir) {
        return calibration::write_speech_calibration_clip(ffmpeg, dir);
      });
  const CalibrationStore store;
  const std::string runtime_id = svp::exec::blake3_prefixed(supplies->runtime.runtime_id);

  if (records.empty()) {
    log("--distributed: no paired workers (pair one with `svp-builder workers pair "
        "<user>@<host>`); OCR runs on this Mac only");
  } else if (!supplies->hello.thread_plan_host_independent) {
    // Every worker would refuse it (plan §3.5); say why once.
    log("warning: --distributed: this build's thread plan leaves a thread count to each "
        "Mac, so its output could depend on which Mac ran a task; OCR runs on this Mac only");
    records.clear();
  }

  // Workers are prepared in parallel with each other and with this Mac's
  // own calibration.
  std::vector<std::future<WorkerOutcome>> pending;
  for (CoordinatorPairingRecord& record : records) {
    pending.push_back(std::async(std::launch::async, [&, record = std::move(record)]() mutable {
      return prepare_worker(std::move(record), *supplies, setup, dispatched,
                            tracking ? &*tracking : nullptr, audio, clip, store, cancellation,
                            *release_);
    }));
  }
  const auto coordinator_start = std::chrono::steady_clock::now();
  DistributedFleet fleet;
  fleet.local_load = local_load;
  try {
    const CalibrationOutcome local =
        ensure_coordinator_calibration(setup, runtime_id, clip, store, cancellation);
    // The record was measured on an otherwise idle Mac, possibly long ago:
    // never run more slots than the memory free right now admits (the same
    // bound the sweep used).
    const svp::exec::worker::HostFacts host = svp::exec::worker::detect_host_facts();
    const std::size_t admitted_now = calibration::ocr_calibration_max_slots(
        svp::exec::worker::sample_memory().available_bytes,
        svp::exec::worker::AdmissionPolicy{}.reserve_bytes(host.physical_memory_bytes),
        host.logical_cpus);
    fleet.coordinator_ocr_slots = std::min(local.ocr.slots, admitted_now);
    fleet.seconds_per_sample = local.ocr.seconds_per_frame;
    log("this Mac: " + std::string(local.measured ? "calibrated now: " : "calibration: ") +
        describe_calibration(local.ocr) +
        (local.measured ? ", in " + seconds_since(coordinator_start) : std::string()));
  } catch (const std::exception& error) {
    // Measuring this Mac failed (its OCR cannot run at all); the build's own
    // OCR tasks will report why. One slot is how a build without
    // measurements runs (kLocalOnlyOcrBatchSlots).
    fleet.coordinator_ocr_slots = 1;
    log(std::string("warning: could not calibrate this Mac's OCR capacity: ") + error.what());
  }
  // The dispatched vision types this Mac measured; a type it cannot measure
  // is not dispatched, and its stage does its work itself as a local build
  // does.
  for (const std::string& type : calibration::dispatched_task_types(work.vision)) {
    try {
      const auto type_start = std::chrono::steady_clock::now();
      const CapacityOutcome capacity =
          ensure_coordinator_capacity(type, dispatched, runtime_id, clip, store,
                                      work.model_cache_root, work.ffmpeg_path, cancellation);
      // As for OCR: never more slots than the memory free right now admits.
      const svp::exec::worker::HostFacts host = svp::exec::worker::detect_host_facts();
      const std::size_t admitted_now = calibration::capacity_max_slots(
          svp::exec::worker::sample_memory().available_bytes,
          svp::exec::worker::AdmissionPolicy{}.reserve_bytes(host.physical_memory_bytes),
          host.logical_cpus, calibration::dispatched_task_peak_rss_mb(type));
      fleet.dispatched_capacity[type] =
          DispatchedTypeCapacity{.coordinator_slots = std::min(capacity.capacity.slots, admitted_now),
                                 .seconds_per_item = capacity.capacity.seconds_per_item};
      log("this Mac: " + std::string(capacity.measured ? "calibrated now: " : "calibration: ") +
          describe_capacity(type, capacity.capacity) +
          (capacity.measured ? ", in " + seconds_since(type_start) : std::string()));
    } catch (const std::exception& error) {
      log("warning: " + type + " is not dispatched: could not calibrate this Mac: " +
          error.what());
    }
  }

  // The audio types on this Mac (M5). asr.chunk_batch is measured now;
  // diarize.window loads sherpa-onnx, which must not load in this process
  // before the build's own ONNX Runtime models, so it is measured in the
  // diarization stage, once sherpa-onnx is loaded there (measure_in_stage).
  const auto admitted_audio_slots = [](std::string_view type, std::size_t measured) {
    const svp::exec::worker::HostFacts host = svp::exec::worker::detect_host_facts();
    return std::min(measured,
                    calibration::capacity_max_slots(
                        svp::exec::worker::sample_memory().available_bytes,
                        svp::exec::worker::AdmissionPolicy{}.reserve_bytes(
                            host.physical_memory_bytes),
                        host.logical_cpus, calibration::audio_task_peak_rss_mb(type)));
  };
  for (const std::string& type : calibration::audio_task_types(audio.setup.audio)) {
    if (type == svp::audio::tasks::kDiarizeWindowTaskType) {
      continue;
    }
    try {
      const auto type_start = std::chrono::steady_clock::now();
      const CapacityOutcome capacity =
          ensure_coordinator_audio_capacity(type, audio.setup, runtime_id, *audio.clip, store,
                                            work.model_cache_root, cancellation);
      fleet.dispatched_capacity[type] =
          DispatchedTypeCapacity{.coordinator_slots = admitted_audio_slots(type, capacity.capacity.slots),
                                 .seconds_per_item = capacity.capacity.seconds_per_item};
      log("this Mac: " + std::string(capacity.measured ? "calibrated now: " : "calibration: ") +
          describe_capacity(type, capacity.capacity) +
          (capacity.measured ? ", in " + seconds_since(type_start) : std::string()));
    } catch (const std::exception& error) {
      log("warning: " + type + " is not dispatched: could not calibrate this Mac: " +
          error.what());
    }
  }
  if (audio.setup.audio.diarization_model_ref) {
    fleet.measure_in_stage =
        [audio, runtime_id, model_cache = work.model_cache_root, quiet = options_.quiet,
         admitted_audio_slots](std::string_view type) -> std::optional<DispatchedTypeCapacity> {
      if (type != svp::audio::tasks::kDiarizeWindowTaskType) {
        return std::nullopt;
      }
      calibration::AudioCalibrationSetup setup = audio.setup;
      const std::optional<std::string> loaded = svp::audio::tasks::loaded_sherpa_library_identity();
      if (!loaded) {
        return std::nullopt;
      }
      // The workers were calibrated, and run windows, against the library
      // named before it loaded. If this process loaded another one (the
      // first candidate failed to load), every window would carry an
      // identity the workers' calibration never saw: map them here instead,
      // as a build without workers does.
      if (*loaded != audio.setup.sherpa_library) {
        if (!quiet) {
          std::cerr << "svp-builder: warning: " << type
                    << " is not dispatched: this Mac loaded sherpa-onnx " << *loaded
                    << ", not the library " << audio.setup.sherpa_library
                    << " the workers were calibrated for\n";
        }
        return std::nullopt;
      }
      try {
        const auto type_start = std::chrono::steady_clock::now();
        const svp::exec::CancellationToken never_cancelled;
        const CalibrationStore store;
        const CapacityOutcome capacity = ensure_coordinator_audio_capacity(
            type, setup, runtime_id, *audio.clip, store, model_cache, never_cancelled);
        if (!quiet) {
          std::cerr << "svp-builder: this Mac: "
                    << (capacity.measured ? "calibrated now: " : "calibration: ")
                    << describe_capacity(type, capacity.capacity)
                    << (capacity.measured ? ", in " + seconds_since(type_start) : std::string())
                    << "\n";
        }
        return DispatchedTypeCapacity{
            .coordinator_slots = admitted_audio_slots(type, capacity.capacity.slots),
            .seconds_per_item = capacity.capacity.seconds_per_item};
      } catch (const std::exception& error) {
        if (!quiet) {
          std::cerr << "svp-builder: warning: " << type
                    << " is not dispatched: could not calibrate this Mac: " << error.what()
                    << "\n";
        }
        return std::nullopt;
      }
    };
  }

  if (tracking) {
    const auto tracking_start = std::chrono::steady_clock::now();
    try {
      const CapacityOutcome local = ensure_coordinator_track_window_calibration(
          tracking->setup, runtime_id, *tracking->clip, cancellation);
      // This Mac's in-process windows have no admission check of their own:
      // run no more at once than the memory free now admits for the build's
      // largest window (workers check each lease against their memory).
      const svp::exec::worker::HostFacts host = svp::exec::worker::detect_host_facts();
      const std::size_t admitted_now = calibration::capacity_max_slots(
          svp::exec::worker::sample_memory().available_bytes,
          svp::exec::worker::AdmissionPolicy{}.reserve_bytes(host.physical_memory_bytes),
          host.logical_cpus, work.tracking->window_peak_rss_mb);
      fleet.coordinator_tracking_slots = std::min(local.capacity.slots, admitted_now);
      fleet.seconds_per_tracking_frame = local.capacity.seconds_per_item;
      log("this Mac: " + std::string(local.measured ? "tracking calibrated now: " : "tracking: ") +
          describe_track_window_calibration(local.capacity) +
          (local.measured ? ", in " + seconds_since(tracking_start) : std::string()));
    } catch (const std::exception& error) {
      // The build's own window tasks will report why tracking cannot run
      // here. One slot is how a build without measurements runs
      // (kCoordinatorTrackWindowSlotsWithoutMeasurement).
      fleet.coordinator_tracking_slots = 1;
      log(std::string("warning: could not calibrate this Mac's tracking capacity: ") +
          error.what());
    }
  }

  std::vector<WorkerOutcome> outcomes;
  for (std::future<WorkerOutcome>& future : pending) {
    outcomes.push_back(future.get());
  }
  if (cancellation.requested()) {
    // The build ends as cancelled right after this; no worker is used.
    // As a build without measurements runs (kLocalOnlyOcrBatchSlots).
    DistributedFleet cancelled{.workers = {}, .coordinator_ocr_slots = 1, .seconds_per_sample = {}};
    cancelled.local_load = local_load;
    return cancelled;
  }
  std::size_t ready = 0;
  std::string problems;
  for (const WorkerOutcome& outcome : outcomes) {
    if (outcome.ready) {
      ++ready;
      log("worker " + worker_name(outcome.record) + ": " + outcome.detail);
    } else {
      log("warning: worker " + worker_name(outcome.record) + " is not used: " + outcome.problem);
      problems += "\n  " + worker_name(outcome.record) + ": " + outcome.problem;
    }
  }
  if (ready < options_.require_workers) {
    throw DistributedPreparationError(
        "--require-workers " + std::to_string(options_.require_workers) + ": only " +
        std::to_string(ready) + " of " + std::to_string(outcomes.size()) +
        " paired worker(s) are ready" + problems);
  }
  if (!outcomes.empty() && ready == 0) {
    log("warning: no worker is ready; OCR runs on this Mac only");
  }

  // Each ready worker gets one executor per graph task type it takes (OCR
  // frame batches, tracking windows): its own session, restricted to that
  // type and sized by that type's measured capacity.
  std::vector<svp::exec::remote::RemoteExecutor*> ocr_remotes;
  const auto add_executor = [this, &supplies](const WorkerOutcome& outcome,
                                              const std::string& suffix, std::size_t slots,
                                              std::string_view task_type,
                                              std::vector<svp::exec::Executor*>& into) {
    auto executor = std::make_unique<svp::exec::remote::RemoteExecutor>(
        svp::exec::remote::RemoteExecutorOptions{
            .executor_id = "worker." + outcome.record.key.pairing_id + suffix,
            .connector = {.pairing = outcome.record.key},
            .slots = slots,
            .session_preamble =
                make_supplying_preamble(declaring_capacity(supplies, task_type, slots))});
    auto restricted = std::make_unique<svp::exec::TaskTypeRestrictedExecutor>(
        *executor, std::set<std::string, std::less<>>{std::string(task_type)});
    into.push_back(restricted.get());
    svp::exec::remote::RemoteExecutor* remote = executor.get();
    remotes_.push_back(std::move(executor));
    restricted_.push_back(std::move(restricted));
    return remote;
  };
  std::vector<FleetDispatchedExecutors::Worker> dispatched_workers;
  for (const WorkerOutcome& outcome : outcomes) {
    if (!outcome.ready) {
      continue;
    }
    if (!outcome.dispatched_slots.empty()) {
      dispatched_workers.push_back(
          {.key = outcome.record.key, .slots = outcome.dispatched_slots});
    }
    if (outcome.ocr_ready) {
      ocr_remotes.push_back(add_executor(outcome, "", outcome.slots,
                                         svp::vision::tasks::kOcrFrameBatchTaskType,
                                         fleet.workers));
    }
    if (tracking && outcome.tracking_ready) {
      add_executor(outcome, ".tracking", outcome.tracking_slots,
                   svp::vision::tasks::kTrackWindowTaskType, fleet.tracking_workers);
    }
  }
  if (!ocr_remotes.empty()) {
    // The executors live as long as this fleet, which outlives the build.
    fleet.release_ocr_workers = [ocr_remotes] {
      for (svp::exec::remote::RemoteExecutor* remote : ocr_remotes) {
        (void)remote->close_idle_session();
      }
    };
  }
  if (!dispatched_workers.empty()) {
    fleet.dispatched_workers =
        std::make_shared<FleetDispatchedExecutors>(std::move(dispatched_workers), supplies,
                                                   release_);
  }
  return fleet;
}

}  // namespace svp::builder::workers
