#include "ocr_calibration_runs.hpp"
#include "worker_restart.hpp"

#include "coordinator_context.hpp"
#include "dispatched_calibration_runs.hpp"
#include "engine/distributed_vision_work.hpp"

#include "svp/builder/build_thread_plan.hpp"
#include "svp/builder/runtime_tools.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cas_store.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_frame_batch_task.hpp"

#include <cstdio>
#include <iostream>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace svp::builder::workers {
namespace {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

constexpr const char* kCalibrationSession = "bs_ocr_calibration";
constexpr const char* kClipMediaType = "application/octet-stream";

svp::vision::tasks::OcrFrameBatchTaskInputs calibration_inputs(
    const OcrCalibrationSetup& setup, const svp::exec::worker::BlobRef& clip) {
  return svp::vision::tasks::OcrFrameBatchTaskInputs{
      .build_session_id = kCalibrationSession,
      .depends_on = {},
      .source = svp::exec::ArtifactRef{
          .blake3 = clip.blake3,
          .bytes = clip.bytes,
          .media_type = kClipMediaType,
          .role = std::string(svp::vision::tasks::kOcrFrameBatchSourceRole)},
      .model_refs = setup.model_refs,
      .pp_ocr = setup.pp_ocr,
      .ffmpeg_build = setup.ffmpeg_build,
      .batch_policy = {},
  };
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open the calibration clip " + path.string());
  }
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad()) {
    throw std::runtime_error("cannot read the calibration clip " + path.string());
  }
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

CalibrationOutcome stored_or_measure(const std::string& name, const CalibrationConditions& wanted,
                                     const CalibrationStore& store,
                                     const std::function<calibration::OcrCalibration()>& measure) {
  if (const std::optional<CalibrationRecord> record = store.read(name);
      record && record->conditions == wanted) {
    return CalibrationOutcome{.ocr = record->ocr, .measured = false};
  }
  CalibrationRecord record{.conditions = wanted,
                           .ocr = measure(),
                           .measured_at = svp::exec::worker::utc_timestamp_now()};
  store.write(name, record);
  return CalibrationOutcome{.ocr = record.ocr, .measured = true};
}

}  // namespace

svp::models::ThreadPlan default_build_thread_plan(const std::filesystem::path& model_cache) {
  BuildPipelineOptions options;
  options.model_cache_dir = model_cache;
  return resolve_build_thread_plan(options, svp::models::detect_host_cpu_topology(),
                                   svp::models::process_environment_lookup(), false)
      .plan;
}

OcrCalibrationSetup default_ocr_calibration_setup(const std::filesystem::path& model_cache) {
  BuildPipelineOptions options;
  options.model_cache_dir = model_cache;
  const svp::models::ThreadPlanResolution plan = resolve_build_thread_plan(
      options, svp::models::detect_host_cpu_topology(), svp::models::process_environment_lookup(),
      false);
  svp::vision::OcrGenerationOptions ocr;
  ocr.model_cache_root = model_cache;
  ocr.performance_profile = options.performance.ocr_performance_profile;
  ocr.recognition_parallel_workers = plan.plan.ocr_recognition_workers;
  ocr.detection_threads = plan.plan.ocr_detection;
  ocr.recognition_threads = plan.plan.ocr_recognition;
  OcrCalibrationSetup setup;
  setup.pp_ocr = svp::vision::make_ocr_pp_ocr_options(ocr);
  setup.model_refs = svp::vision::tasks::ocr_frame_batch_model_refs(setup.pp_ocr);
  setup.ffmpeg_path = resolve_runtime_tool(RuntimeTool::ffmpeg, std::nullopt,
                                           locate_runtime_bundle(current_executable()),
                                           process_environment())
                          .path;
  const std::optional<std::string> build =
      svp::vision::tasks::ffmpeg_build_identity(setup.ffmpeg_path);
  if (!build) {
    throw std::runtime_error("cannot run ffmpeg (" + setup.ffmpeg_path.string() +
                             ") to calibrate OCR; pass it with $SVP_FFMPEG");
  }
  setup.ffmpeg_build = *build;
  return setup;
}

CalibrationClipFile::CalibrationClipFile(std::filesystem::path ffmpeg)
    : CalibrationClipFile(std::move(ffmpeg),
                          [](const std::filesystem::path& tool, const std::filesystem::path& dir) {
                            return svp::vision::tasks::write_ocr_calibration_clip(tool, dir).path;
                          }) {}

CalibrationClipFile::CalibrationClipFile(std::filesystem::path ffmpeg, Writer writer)
    : ffmpeg_(std::move(ffmpeg)), writer_(std::move(writer)) {}

CalibrationClipFile::~CalibrationClipFile() {
  if (!directory_.empty()) {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
}

svp::exec::worker::BlobSource CalibrationClipFile::blob() {
  const std::lock_guard lock(mutex_);
  if (!blob_) {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "svp-ocr-calibration-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw WorkerError(WorkerErrorCode::io, "cannot create a calibration directory");
    }
    directory_ = pattern;
    blob_ = svp::exec::worker::describe_blob_file(writer_(ffmpeg_, directory_));
  }
  return *blob_;
}

CalibrationConditions calibration_conditions(const OcrCalibrationSetup& setup,
                                             const std::string& runtime_id,
                                             const svp::exec::worker::HostFacts& host) {
  // The digest does not depend on the clip's bytes (only on its frame
  // timestamps and size), so any well-formed source ref serves.
  const svp::exec::TaskSpec spec = calibration::ocr_calibration_spec(
      calibration_inputs(setup, svp::exec::worker::BlobRef{.blake3 = {}, .bytes = 1}),
      svp::vision::tasks::ocr_calibration_timestamps_us());
  std::string bundles;
  for (const svp::exec::TaskModelRef& ref : setup.model_refs) {
    bundles += (bundles.empty() ? "" : ",") + ref.model_bundle_id;
  }
  return CalibrationConditions{
      .runtime_id = runtime_id,
      .os = host.os,
      .cpu_brand = host.cpu_brand,
      .logical_cpus = host.logical_cpus,
      .physical_memory_bytes = host.physical_memory_bytes,
      .parameters_blake3 = svp::exec::blake3_prefixed(spec.parameters_blake3) + "+recipe" +
                           std::to_string(svp::vision::tasks::kOcrCalibrationRecipeVersion),
      .model_bundles = bundles};
}

CalibrationOutcome ensure_coordinator_calibration(const OcrCalibrationSetup& setup,
                                                  const std::string& runtime_id,
                                                  CalibrationClipFile& clip,
                                                  const CalibrationStore& store,
                                                  const svp::exec::CancellationToken& cancellation) {
  const svp::exec::worker::HostFacts host = svp::exec::worker::detect_host_facts();
  return stored_or_measure(
      std::string(kCoordinatorCalibrationName), calibration_conditions(setup, runtime_id, host),
      store, [&] {
        const svp::exec::worker::BlobSource source = clip.blob();
        std::string cas_pattern =
            (std::filesystem::temp_directory_path() / "svp-ocr-calibration-cas-XXXXXX").string();
        if (::mkdtemp(cas_pattern.data()) == nullptr) {
          throw WorkerError(WorkerErrorCode::io, "cannot create a calibration cache");
        }
        const std::filesystem::path cas_root = cas_pattern;
        struct Remove {
          std::filesystem::path path;
          ~Remove() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
          }
        } remove{cas_root};
        svp::exec::CacheResult<svp::exec::CasStore> cas = svp::exec::CasStore::at(cas_root);
        if (!cas) {
          throw WorkerError(WorkerErrorCode::io, "calibration cache: " + cas.error().message);
        }
        svp::exec::CasTaskArtifactAccess artifacts(std::move(cas).value(), kCalibrationSession);
        const std::vector<std::byte> bytes = read_file(source.file);
        if (bytes.size() != source.ref.bytes) {
          throw std::runtime_error("the calibration clip changed size while it was in use");
        }
        (void)artifacts.put(bytes, kClipMediaType,
                            std::string(svp::vision::tasks::kOcrFrameBatchSourceRole));
        svp::exec::TaskTypeRegistry registry;
        svp::vision::tasks::register_ocr_frame_batch_task(
            registry, svp::vision::tasks::OcrFrameBatchWorkerEnvironment{
                          .model_cache_root = setup.pp_ocr.model_cache_root,
                          .ffmpeg_path = setup.ffmpeg_path,
                          .write_output =
                              [&artifacts](std::span<const std::byte> out, std::string media_type,
                                           std::string role) {
                                return artifacts.put(out, std::move(media_type), std::move(role));
                              },
                          .model_cache_for = {}});
        const svp::exec::worker::AdmissionPolicy admission;
        const std::size_t max_slots = calibration::ocr_calibration_max_slots(
            svp::exec::worker::sample_memory().available_bytes,
            admission.reserve_bytes(host.physical_memory_bytes), host.logical_cpus);
        svp::exec::InProcessExecutor executor(
            registry, artifacts,
            svp::exec::InProcessExecutorOptions{.executor_id = "calibration",
                                                .threads = max_slots,
                                                .worker_session_id = "ws_ocr_calibration",
                                                .runtime_id = {}});
        return calibration::calibrate_ocr_capacity(executor, max_slots,
                                                   calibration_inputs(setup, source.ref),
                                                   svp::vision::tasks::ocr_calibration_timestamps_us(),
                                                   cancellation);
      });
}

CalibrationOutcome ensure_worker_calibration(const svp::exec::remote::PairingKey& pairing,
                                             const WorkerSupplies& supplies,
                                             const svp::exec::worker::WorkerHelloAck& ack,
                                             const OcrCalibrationSetup& setup,
                                             CalibrationClipFile& clip,
                                             const CalibrationStore& store,
                                             const svp::exec::CancellationToken& cancellation) {
  return stored_or_measure(
      pairing.pairing_id,
      calibration_conditions(setup, svp::exec::blake3_prefixed(supplies.runtime.runtime_id),
                             ack.host),
      store, [&] {
        const svp::exec::worker::BlobSource source = clip.blob();
        auto with_clip = std::make_shared<WorkerSupplies>(supplies);
        with_clip->blobs = {source};
        const std::size_t max_slots = calibration::ocr_calibration_max_slots(
            ack.memory.available_bytes, ack.memory_reserve_bytes, ack.host.logical_cpus);
        svp::exec::remote::RemoteExecutor executor(with_supplied_sessions(
            svp::exec::remote::RemoteExecutorOptions{.executor_id = "calibration." + pairing.pairing_id,
                                                     .connector = {.pairing = pairing},
                                                     .slots = max_slots},
            with_clip));
        return calibration::calibrate_ocr_capacity(executor, max_slots,
                                                   calibration_inputs(setup, source.ref),
                                                   svp::vision::tasks::ocr_calibration_timestamps_us(),
                                                   cancellation);
      });
}

std::string describe_calibration(const calibration::OcrCalibration& ocr) {
  std::ostringstream out;
  out << ocr.slots << " OCR slot(s), " << ocr.seconds_per_frame << " s per frame per slot (";
  for (std::size_t index = 0; index < ocr.sweep.size(); ++index) {
    const calibration::OcrCalibrationStep& step = ocr.sweep[index];
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%.2f", step.frames_per_second);
    out << (index == 0 ? "" : ", ") << step.slots << " slot(s) " << rate << " frames/s";
  }
  out << "; " << ocr.stopped_because << ")";
  return out.str();
}

bool calibrate_for_workers_command(const svp::exec::worker::CoordinatorPairingRecord& record,
                                   const svp::exec::worker::CoordinatorHello& hello,
                                   const svp::exec::worker::CoordinatorRuntime& runtime,
                                   const std::filesystem::path& model_cache) {
  try {
    const OcrCalibrationSetup setup = default_ocr_calibration_setup(model_cache);
    // The default build's dispatched vision work (M4), measured beside OCR.
    const calibration::DispatchedCalibrationSetup dispatched{
        .pp_ocr = setup.pp_ocr,
        .pp_ocr_model_refs = setup.model_refs,
        .vision = engine::plan_distributed_vision_work(model_cache,
                                                       default_build_thread_plan(model_cache)),
        .ffmpeg_build = setup.ffmpeg_build};
    WorkerSupplies supplies;
    supplies.hello = hello;
    supplies.runtime = runtime;
    std::vector<std::string> model_ids;
    for (const svp::exec::TaskModelRef& ref : setup.model_refs) {
      model_ids.push_back(ref.model_id);
    }
    for (const std::optional<DistributedOnnxWork>* onnx :
         {&dispatched.vision.text_embeddings, &dispatched.vision.keyframe_embeddings,
          &dispatched.vision.depth}) {
      if (*onnx) {
        model_ids.push_back((*onnx)->model_ref.model_id);
      }
    }
    supplies.models = svp::exec::worker::prepare_model_bundles(model_cache, model_ids);
    CalibrationClipFile clip(setup.ffmpeg_path);
    const CalibrationStore store;
    const svp::exec::CancellationToken never_cancelled;
    const CalibrationOutcome local = ensure_coordinator_calibration(
        setup, svp::exec::blake3_prefixed(runtime.runtime_id), clip, store, never_cancelled);
    std::cout << "this Mac: " << (local.measured ? "calibrated: " : "calibration current: ")
              << describe_calibration(local.ocr) << "\n";
    svp::exec::worker::WorkerHelloAck ack;
    {
      const std::unique_ptr<svp::exec::worker::WorkerConnection> connection =
          svp::exec::worker::connect_to_worker(record.key);
      svp::exec::worker::WorkerSessionClient client(*connection->reader, *connection->writer);
      ack = supply_worker_session(*connection->reader, *connection->writer, supplies).ack;
      client.shutdown();
    }
    if (const std::optional<svp::exec::worker::WorkerHelloAck> restarted =
            await_worker_runtime_switch(record.key, supplies.hello, supplies.runtime, ack, {},
                                        [](const std::string& line) { std::cout << line << "\n"; })) {
      ack = *restarted;
    }
    const CalibrationOutcome worker = ensure_worker_calibration(
        record.key, supplies, ack, setup, clip, store, never_cancelled);
    std::cout << "worker: " << (worker.measured ? "calibrated: " : "calibration current: ")
              << describe_calibration(worker.ocr) << "\n";
    // The dispatched types are measured best effort: the command's outcome
    // stays OCR's, and a distributed build measures any type missing here.
    for (const std::string& type : calibration::dispatched_task_types(dispatched.vision)) {
      try {
        const CapacityOutcome local_capacity = ensure_coordinator_capacity(
            type, dispatched, svp::exec::blake3_prefixed(runtime.runtime_id), clip, store,
            model_cache, setup.ffmpeg_path, never_cancelled);
        std::cout << "this Mac: "
                  << (local_capacity.measured ? "calibrated: " : "calibration current: ")
                  << describe_capacity(type, local_capacity.capacity) << "\n";
        const CapacityOutcome worker_capacity =
            ensure_worker_capacity(record.key, supplies, ack, type, dispatched, clip,
                                   setup.ffmpeg_path, store, never_cancelled);
        std::cout << "worker: "
                  << (worker_capacity.measured ? "calibrated: " : "calibration current: ")
                  << describe_capacity(type, worker_capacity.capacity) << "\n";
      } catch (const std::exception& error) {
        std::cerr << "svp-builder: warning: " << type << " capacity calibration failed: "
                  << error.what() << "\n  a --distributed build measures it again\n";
      }
    }
    return true;
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: OCR capacity calibration failed: " << error.what()
              << "\n  `svp-builder workers sync` measures it again; a --distributed build "
                 "measures a worker it has no current calibration for\n";
    return false;
  }
}

}  // namespace svp::builder::workers
