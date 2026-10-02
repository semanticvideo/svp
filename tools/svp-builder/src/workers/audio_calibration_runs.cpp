#include "audio_calibration_runs.hpp"

#include "capacity_record_support.hpp"

#include "svp/audio/tasks/asr_chunk_batch.hpp"
#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/audio/whisper_cpp_backend.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cas_store.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/worker_error.hpp"

namespace svp::builder::workers {
namespace {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

constexpr const char* kCalibrationSession = "bs_audio_calibration";

svp::exec::ArtifactRef clip_ref(const svp::exec::worker::BlobRef& blob) {
  return svp::exec::ArtifactRef{.blake3 = blob.blake3,
                                .bytes = blob.bytes,
                                .media_type = std::string(svp::audio::tasks::kAudioTaskInputMediaType),
                                .role = std::string(svp::audio::tasks::kAudioTaskInputRole)};
}

std::string model_bundles(std::string_view task_type,
                          const calibration::AudioCalibrationSetup& setup) {
  std::string bundles;
  if (task_type == svp::audio::tasks::kAsrChunkBatchTaskType) {
    for (const svp::exec::TaskModelRef& ref : setup.audio.asr_model_refs) {
      bundles += (bundles.empty() ? "" : ",") + ref.model_bundle_id;
    }
  } else if (setup.audio.diarization_model_ref) {
    bundles = setup.audio.diarization_model_ref->model_bundle_id;
  }
  return bundles;
}

CalibrationConditions audio_conditions(std::string_view task_type,
                                       const calibration::AudioCalibrationSetup& setup,
                                       const svp::exec::ArtifactRef& clip,
                                       const std::string& runtime_id,
                                       const svp::exec::worker::HostFacts& host) {
  return CalibrationConditions{
      .runtime_id = runtime_id,
      .os = host.os,
      .cpu_brand = host.cpu_brand,
      .logical_cpus = host.logical_cpus,
      .physical_memory_bytes = host.physical_memory_bytes,
      .parameters_blake3 = svp::exec::blake3_prefixed(svp::exec::compute_parameters_blake3(
          calibration::audio_capacity_settings(task_type, setup, clip))),
      .model_bundles = model_bundles(task_type, setup)};
}

}  // namespace

CapacityOutcome ensure_coordinator_audio_capacity(std::string_view task_type,
                                                  const calibration::AudioCalibrationSetup& setup,
                                                  const std::string& runtime_id,
                                                  CalibrationClipFile& clip,
                                                  const CalibrationStore& store,
                                                  const std::filesystem::path& model_cache,
                                                  const svp::exec::CancellationToken& cancellation) {
  const svp::exec::worker::HostFacts host = svp::exec::worker::detect_host_facts();
  const svp::exec::worker::BlobSource source = clip.blob();
  const svp::exec::ArtifactRef ref = clip_ref(source.ref);
  return stored_or_measured_capacity(
      std::string(kCoordinatorCalibrationName), task_type,
      audio_conditions(task_type, setup, ref, runtime_id, host), store, [&] {
        const CalibrationTemporaryDirectory cas_root("svp-audio-calibration-cas");
        const CalibrationTemporaryDirectory scratch("svp-audio-calibration-scratch");
        svp::exec::CacheResult<svp::exec::CasStore> cas = svp::exec::CasStore::at(cas_root.path());
        if (!cas) {
          throw WorkerError(WorkerErrorCode::io, "calibration cache: " + cas.error().message);
        }
        svp::exec::CasTaskArtifactAccess artifacts(std::move(cas).value(), kCalibrationSession);
        const std::vector<std::byte> bytes = read_calibration_clip(source.file);
        if (bytes.size() != source.ref.bytes) {
          throw std::runtime_error("the calibration clip changed size while it was in use");
        }
        (void)artifacts.put(bytes, ref.media_type, ref.role);
        svp::exec::TaskTypeRegistry registry;
        // A calibration task that cannot run fails (record_start_failures
        // false): a measurement of work that did not happen is void.
        svp::audio::tasks::register_audio_tasks(
            registry, svp::audio::tasks::AudioTaskEnvironment{
                          .model_cache_root = model_cache,
                          .model_cache_for = {},
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
            calibration::audio_task_peak_rss_mb(task_type));
        // The models a measurement loads stay cached in this process until
        // released; the build's own ASR loads them again when it runs.
        struct ReleaseAsrModels {
          ~ReleaseAsrModels() {
            svp::audio::release_whisper_cpp_model();
            svp::audio::release_phoneme_aligner();
          }
        } release;
        svp::exec::InProcessExecutor executor(
            registry, artifacts,
            svp::exec::InProcessExecutorOptions{.executor_id = "calibration",
                                                .threads = max_slots,
                                                .worker_session_id = "ws_audio_calibration",
                                                .runtime_id = {}});
        return calibration::calibrate_capacity(
            executor, max_slots,
            calibration::audio_capacity_workload(task_type, setup, ref, source.file),
            cancellation);
      });
}

CapacityOutcome ensure_worker_audio_capacity(const svp::exec::remote::PairingKey& pairing,
                                             const WorkerSupplies& supplies,
                                             const svp::exec::worker::WorkerHelloAck& ack,
                                             std::string_view task_type,
                                             const calibration::AudioCalibrationSetup& setup,
                                             CalibrationClipFile& clip,
                                             const CalibrationStore& store,
                                             const svp::exec::CancellationToken& cancellation) {
  const svp::exec::worker::BlobSource source = clip.blob();
  const svp::exec::ArtifactRef ref = clip_ref(source.ref);
  return stored_or_measured_capacity(
      pairing.pairing_id, task_type,
      audio_conditions(task_type, setup, ref,
                       svp::exec::blake3_prefixed(supplies.runtime.runtime_id), ack.host),
      store, [&] {
        auto with_clip = std::make_shared<WorkerSupplies>(supplies);
        with_clip->blobs = {source};
        const std::size_t max_slots = calibration::capacity_max_slots(
            ack.memory.available_bytes, ack.memory_reserve_bytes, ack.host.logical_cpus,
            calibration::audio_task_peak_rss_mb(task_type));
        svp::exec::remote::RemoteExecutor executor(svp::exec::remote::RemoteExecutorOptions{
            .executor_id = "calibration." + pairing.pairing_id,
            .connector = {.pairing = pairing},
            .slots = max_slots,
            .session_preamble = make_supplying_preamble(with_clip)});
        return calibration::calibrate_capacity(
            executor, max_slots,
            calibration::audio_capacity_workload(task_type, setup, ref, source.file),
            cancellation);
      });
}

}  // namespace svp::builder::workers
