#include "dispatched_calibration_runs.hpp"

#include "capacity_record_support.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cas_store.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/vision/tasks/dispatched_vision_tasks.hpp"
#include "svp/vision/tasks/embed_keyframe_batch_parameters.hpp"
#include "svp/vision/tasks/embed_text_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/pp_ocr_parameters.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace svp::builder::workers {
namespace {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

constexpr const char* kCalibrationSession = "bs_dispatch_calibration";
constexpr const char* kClipMediaType = "application/octet-stream";

svp::exec::ArtifactRef clip_ref(const svp::exec::worker::BlobRef& blob) {
  return svp::exec::ArtifactRef{.blake3 = blob.blake3,
                                .bytes = blob.bytes,
                                .media_type = kClipMediaType,
                                .role = std::string(svp::vision::tasks::kOcrFrameBatchSourceRole)};
}

nlohmann::json model_json(const DistributedOnnxWork& work) {
  return {{"execution_provider", work.model.execution_provider},
          {"inter_op", work.model.threads.inter_op},
          {"intra_op", work.model.threads.intra_op},
          {"model_bundle_id", work.model_ref.model_bundle_id}};
}

// What a type's measurement depends on besides the Mac: its settings, the
// decoder, and the workload recipe (never the clip's bytes, which the recipe
// and decoder fix).
CalibrationConditions capacity_conditions(std::string_view task_type,
                                          const calibration::DispatchedCalibrationSetup& setup,
                                          const std::string& runtime_id,
                                          const svp::exec::worker::HostFacts& host) {
  nlohmann::json settings = {{"embedding_dim", setup.vision.embedding_dim},
                             {"ffmpeg_build", setup.ffmpeg_build},
                             {"recipe", calibration::kDispatchedCalibrationRecipeVersion},
                             {"task_type", std::string(task_type)}};
  std::string bundles;
  if (task_type == svp::vision::tasks::kOcrCropBatchTaskType) {
    settings["pp_ocr"] = svp::vision::tasks::pp_ocr_parameter_fields(setup.pp_ocr);
    for (const svp::exec::TaskModelRef& ref : setup.pp_ocr_model_refs) {
      bundles += (bundles.empty() ? "" : ",") + ref.model_bundle_id;
    }
  } else {
    const std::optional<DistributedOnnxWork>& work =
        task_type == svp::vision::tasks::kEmbedTextBatchTaskType ? setup.vision.text_embeddings
        : task_type == svp::vision::tasks::kEmbedKeyframeBatchTaskType
            ? setup.vision.keyframe_embeddings
            : setup.vision.depth;
    if (!work) {
      throw std::runtime_error("this build does not dispatch " + std::string(task_type));
    }
    settings["model"] = model_json(*work);
    bundles = work->model_ref.model_bundle_id;
  }
  return CalibrationConditions{
      .runtime_id = runtime_id,
      .os = host.os,
      .cpu_brand = host.cpu_brand,
      .logical_cpus = host.logical_cpus,
      .physical_memory_bytes = host.physical_memory_bytes,
      .parameters_blake3 =
          svp::exec::blake3_prefixed(svp::exec::compute_parameters_blake3(settings)),
      .model_bundles = bundles};
}

}  // namespace

CapacityOutcome ensure_coordinator_capacity(std::string_view task_type,
                                            const calibration::DispatchedCalibrationSetup& setup,
                                            const std::string& runtime_id,
                                            CalibrationClipFile& clip,
                                            const CalibrationStore& store,
                                            const std::filesystem::path& model_cache,
                                            const std::filesystem::path& ffmpeg,
                                            const svp::exec::CancellationToken& cancellation) {
  const svp::exec::worker::HostFacts host = svp::exec::worker::detect_host_facts();
  return stored_or_measured_capacity(
      std::string(kCoordinatorCalibrationName), task_type,
      capacity_conditions(task_type, setup, runtime_id, host), store, [&] {
        const svp::exec::worker::BlobSource source = clip.blob();
        const CalibrationTemporaryDirectory cas_root("svp-dispatch-calibration-cas");
        const CalibrationTemporaryDirectory scratch("svp-dispatch-calibration-scratch");
        svp::exec::CacheResult<svp::exec::CasStore> cas = svp::exec::CasStore::at(cas_root.path());
        if (!cas) {
          throw WorkerError(WorkerErrorCode::io, "calibration cache: " + cas.error().message);
        }
        svp::exec::CasTaskArtifactAccess artifacts(std::move(cas).value(), kCalibrationSession);
        const std::vector<std::byte> bytes = read_calibration_clip(source.file);
        if (bytes.size() != source.ref.bytes) {
          throw std::runtime_error("the calibration clip changed size while it was in use");
        }
        (void)artifacts.put(bytes, kClipMediaType,
                            std::string(svp::vision::tasks::kOcrFrameBatchSourceRole));
        svp::exec::TaskTypeRegistry registry;
        // A calibration task that cannot run fails (record_start_failures
        // false): a measurement of work that did not happen is void.
        svp::vision::tasks::register_dispatched_vision_tasks(
            registry, svp::vision::tasks::DispatchedTaskEnvironment{
                          .model_cache_root = model_cache,
                          .model_cache_for = {},
                          .ffmpeg_path = ffmpeg,
                          .scratch_dir = scratch.path(),
                          .write_output =
                              [&artifacts](std::span<const std::byte> out, std::string media_type,
                                           std::string role) {
                                return artifacts.put(out, std::move(media_type), std::move(role));
                              },
                          .record_start_failures = false});
        const svp::exec::worker::AdmissionPolicy admission;
        const std::size_t max_slots = calibration::capacity_max_slots(
            svp::exec::worker::sample_memory().available_bytes,
            admission.reserve_bytes(host.physical_memory_bytes), host.logical_cpus,
            calibration::dispatched_task_peak_rss_mb(task_type));
        svp::exec::InProcessExecutor executor(
            registry, artifacts,
            svp::exec::InProcessExecutorOptions{.executor_id = "calibration",
                                                .threads = max_slots,
                                                .worker_session_id = "ws_dispatch_calibration",
                                                .runtime_id = {}});
        return calibration::calibrate_capacity(
            executor, max_slots,
            calibration::dispatched_capacity_workload(task_type, setup, clip_ref(source.ref),
                                                      source.file, ffmpeg),
            cancellation);
      });
}

CapacityOutcome ensure_worker_capacity(const svp::exec::remote::PairingKey& pairing,
                                       const WorkerSupplies& supplies,
                                       const svp::exec::worker::WorkerHelloAck& ack,
                                       std::string_view task_type,
                                       const calibration::DispatchedCalibrationSetup& setup,
                                       CalibrationClipFile& clip,
                                       const std::filesystem::path& ffmpeg,
                                       const CalibrationStore& store,
                                       const svp::exec::CancellationToken& cancellation) {
  return stored_or_measured_capacity(
      pairing.pairing_id, task_type,
      capacity_conditions(task_type, setup,
                          svp::exec::blake3_prefixed(supplies.runtime.runtime_id), ack.host),
      store, [&] {
        const svp::exec::worker::BlobSource source = clip.blob();
        auto with_clip = std::make_shared<WorkerSupplies>(supplies);
        with_clip->blobs = {source};
        const std::size_t max_slots = calibration::capacity_max_slots(
            ack.memory.available_bytes, ack.memory_reserve_bytes, ack.host.logical_cpus,
            calibration::dispatched_task_peak_rss_mb(task_type));
        svp::exec::remote::RemoteExecutor executor(svp::exec::remote::RemoteExecutorOptions{
            .executor_id = "calibration." + pairing.pairing_id,
            .connector = {.pairing = pairing},
            .slots = max_slots,
            .session_preamble = make_supplying_preamble(with_clip)});
        return calibration::calibrate_capacity(
            executor, max_slots,
            calibration::dispatched_capacity_workload(task_type, setup, clip_ref(source.ref),
                                                      source.file, ffmpeg),
            cancellation);
      });
}

std::string describe_capacity(std::string_view task_type,
                              const calibration::CapacityCalibration& capacity) {
  std::ostringstream out;
  out << task_type << " " << capacity.slots << " slot(s), " << capacity.seconds_per_item
      << " s per item per slot (";
  for (std::size_t index = 0; index < capacity.sweep.size(); ++index) {
    const calibration::CapacityStep& step = capacity.sweep[index];
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%.2f", step.items_per_second);
    out << (index == 0 ? "" : ", ") << step.slots << ": " << rate << "/s";
  }
  out << "; " << capacity.stopped_because << ")";
  return out.str();
}

}  // namespace svp::builder::workers
