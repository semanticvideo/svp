#include "distributed_fleet.hpp"

#include "coordinator_context.hpp"
#include "worker_reach.hpp"

#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"

#include <chrono>
#include <future>
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
  std::string detail;
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
std::unique_ptr<WorkerConnection> connect_within_window(const remote::PairingKey& key) {
  const auto give_up =
      std::chrono::steady_clock::now() + remote::kDefaultReconnectWindow;
  while (true) {
    try {
      return connect_to_worker(key);
    } catch (const remote::RemoteTransportError& error) {
      if (error.code() == remote::RemoteErrorCode::authentication_failed ||
          std::chrono::steady_clock::now() + remote::kDefaultReconnectPause >= give_up) {
        throw;
      }
    }
    std::this_thread::sleep_for(remote::kDefaultReconnectPause);
  }
}

WorkerOutcome prepare_worker(CoordinatorPairingRecord record, const WorkerSupplies& supplies,
                             const OcrCalibrationSetup& setup, CalibrationClipFile& clip,
                             const CalibrationStore& store,
                             const svp::exec::CancellationToken& cancellation) {
  WorkerOutcome outcome{.record = std::move(record)};
  const auto start = std::chrono::steady_clock::now();
  try {
    WorkerHelloAck ack;
    TransferStats stats;
    std::string route;
    {
      const std::unique_ptr<WorkerConnection> connection = connect_within_window(outcome.record.key);
      route = connection->connection.route.route.interface_name + " (" +
              std::string(remote::route_medium_name(connection->connection.route.route.medium)) +
              ")";
      WorkerSessionClient client(*connection->reader, *connection->writer);
      const SuppliedSession session =
          supply_worker_session(*connection->reader, *connection->writer, supplies);
      ack = session.ack;
      stats = session.stats;
      client.shutdown();
    }
    const CalibrationOutcome calibration =
        ensure_worker_calibration(outcome.record.key, supplies, ack, setup, clip, store,
                                  cancellation);
    outcome.ready = true;
    outcome.slots = calibration.ocr.slots;
    std::ostringstream detail;
    detail << "via " << route << ", sent " << format_bytes(stats.bytes_sent) << " ("
           << (stats.runtime_pushed ? "runtime, " : "") << stats.model_bundles_pushed.size()
           << " model bundle(s), " << stats.blobs_sent << " blob(s)), "
           << (calibration.measured ? "calibrated now: " : "calibration: ")
           << describe_calibration(calibration.ocr) << ", ready in " << seconds_since(start);
    outcome.detail = detail.str();
  } catch (const std::exception& error) {
    outcome.problem = error.what();
  }
  return outcome;
}

std::string worker_name(const CoordinatorPairingRecord& record) {
  return record.key.pairing_id + " (" + record.worker.ssh_target + ")";
}

}  // namespace

PairedWorkerFleet::PairedWorkerFleet(DistributedFleetOptions options)
    : options_(std::move(options)) {}

PairedWorkerFleet::~PairedWorkerFleet() = default;

DistributedFleet PairedWorkerFleet::prepare(const DistributedOcrWork& work) {
  const auto log = [this](const std::string& line) {
    if (!options_.quiet) {
      std::cerr << "svp-builder: " << line << "\n";
    }
  };
  const PairingDirectory pairings(default_coordinator_pairings_dir());
  std::vector<CoordinatorPairingRecord> records = load_coordinator_pairings(pairings);

  auto supplies = std::make_shared<WorkerSupplies>();
  supplies->runtime = locate_coordinator_runtime(current_executable());
  supplies->hello = make_coordinator_hello(supplies->runtime, work.thread_plan,
                                           model_set_summary(work.model_cache_root));
  std::vector<std::string> model_ids;
  for (const svp::exec::TaskModelRef& ref : work.model_refs) {
    model_ids.push_back(ref.model_id);
  }
  supplies->models = prepare_model_bundles(work.model_cache_root, model_ids);
  supplies->blobs = {BlobSource{.ref = BlobRef{.blake3 = work.source.blake3,
                                               .bytes = work.source.bytes},
                                .file = work.source_path}};

  const OcrCalibrationSetup setup{.pp_ocr = work.pp_ocr,
                                  .model_refs = work.model_refs,
                                  .ffmpeg_path = work.ffmpeg_path,
                                  .ffmpeg_build = work.ffmpeg_build};
  CalibrationClipFile clip(work.ffmpeg_path);
  const CalibrationStore store;
  const svp::exec::CancellationToken never_cancelled;
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
      return prepare_worker(std::move(record), *supplies, setup, clip, store, never_cancelled);
    }));
  }
  const auto coordinator_start = std::chrono::steady_clock::now();
  DistributedFleet fleet;
  try {
    const CalibrationOutcome local =
        ensure_coordinator_calibration(setup, runtime_id, clip, store, never_cancelled);
    fleet.coordinator_ocr_slots = local.ocr.slots;
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

  std::vector<WorkerOutcome> outcomes;
  for (std::future<WorkerOutcome>& future : pending) {
    outcomes.push_back(future.get());
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

  for (const WorkerOutcome& outcome : outcomes) {
    if (!outcome.ready) {
      continue;
    }
    auto executor = std::make_unique<svp::exec::remote::RemoteExecutor>(
        svp::exec::remote::RemoteExecutorOptions{
            .executor_id = "worker." + outcome.record.key.pairing_id,
            .connector = {.pairing = outcome.record.key},
            .slots = outcome.slots,
            .session_preamble = make_supplying_preamble(supplies)});
    auto restricted = std::make_unique<svp::exec::TaskTypeRestrictedExecutor>(
        *executor, std::set<std::string, std::less<>>{
                       std::string(svp::vision::tasks::kOcrFrameBatchTaskType)});
    fleet.workers.push_back(restricted.get());
    remotes_.push_back(std::move(executor));
    restricted_.push_back(std::move(restricted));
  }
  return fleet;
}

}  // namespace svp::builder::workers
