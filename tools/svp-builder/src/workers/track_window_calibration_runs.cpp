#include "track_window_calibration_runs.hpp"

#include "calibration/track_window_capacity_calibration.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cas_store.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/vision/tasks/track_window_calibration_clip.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"
#include "svp/vision/tasks/track_window_task.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace svp::builder::workers {
namespace {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

constexpr const char* kCalibrationSession = "bs_track_window_calibration";
constexpr const char* kClipMediaType = "application/octet-stream";

svp::vision::tasks::TrackWindowTaskInputs calibration_inputs(
    const TrackWindowCalibrationSetup& setup, const svp::exec::worker::BlobRef& clip) {
  return svp::vision::tasks::TrackWindowTaskInputs{
      .build_session_id = kCalibrationSession,
      .depends_on = {},
      .source = svp::exec::ArtifactRef{
          .blake3 = clip.blake3,
          .bytes = clip.bytes,
          .media_type = kClipMediaType,
          .role = std::string(svp::vision::tasks::kTrackWindowSourceRole)},
      .model_refs = setup.model_refs,
      .options = setup.options,
      .frame_width = svp::vision::tasks::kTrackWindowCalibrationFrameWidth,
      .frame_height = svp::vision::tasks::kTrackWindowCalibrationFrameHeight,
      .ffmpeg_build = setup.ffmpeg_build,
      .cost = {},
  };
}

std::int64_t sample_interval_us(const TrackWindowCalibrationSetup& setup) {
  return svp::vision::visual_tracking_quality_policy(setup.options.quality).sample_interval_us;
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

// Hex digits of the workload digest in a record's task type: 64 bits, so
// distinct workloads of one Mac never share a record by accident.
constexpr std::size_t kWorkloadNameHexDigits = 16;

// The record's task type: track.window and the workload, so each tracking
// quality (and every other change to the window's work) keeps its own
// measurement beside the others.
std::string record_type(const CalibrationConditions& conditions) {
  return std::string(svp::vision::tasks::kTrackWindowTaskType) + "." +
         svp::exec::blake3_hex(svp::exec::blake3_digest(conditions.parameters_blake3))
             .substr(0, kWorkloadNameHexDigits);
}

CapacityOutcome stored_or_measure(const std::string& name, const CalibrationConditions& wanted,
                                  const std::function<calibration::CapacityCalibration()>& measure) {
  const CalibrationStore store;
  const std::string type = record_type(wanted);
  if (const std::optional<CapacityRecord> record = store.read_capacity(name, type);
      record && record->conditions == wanted) {
    return CapacityOutcome{.capacity = record->capacity, .measured = false};
  }
  CapacityRecord record{.conditions = wanted,
                        .capacity = measure(),
                        .measured_at = svp::exec::worker::utc_timestamp_now()};
  store.write_capacity(name, type, record);
  return CapacityOutcome{.capacity = record.capacity, .measured = true};
}

}  // namespace

std::unique_ptr<CalibrationClipFile> track_window_calibration_clip_file(
    const TrackWindowCalibrationSetup& setup) {
  const std::int64_t interval = sample_interval_us(setup);
  return std::make_unique<CalibrationClipFile>(
      setup.ffmpeg_path,
      [interval](const std::filesystem::path& ffmpeg, const std::filesystem::path& directory) {
        return svp::vision::tasks::write_track_window_calibration_clip(ffmpeg, directory,
                                                                       interval);
      });
}

CalibrationConditions track_window_calibration_conditions(
    const TrackWindowCalibrationSetup& setup, const std::string& runtime_id,
    const svp::exec::worker::HostFacts& host) {
  // The digest does not depend on the clip's bytes (only on its frame
  // timestamps and size), so any well-formed source ref serves.
  const calibration::CapacityWorkload workload = calibration::track_window_calibration_workload(
      calibration_inputs(setup, svp::exec::worker::BlobRef{.blake3 = {}, .bytes = 1}));
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
      .parameters_blake3 =
          svp::exec::blake3_prefixed(workload.batch.parameters_blake3) + "+recipe" +
          std::to_string(svp::vision::tasks::kTrackWindowCalibrationRecipeVersion),
      .model_bundles = bundles};
}

CapacityOutcome ensure_coordinator_track_window_calibration(
    const TrackWindowCalibrationSetup& setup, const std::string& runtime_id,
    CalibrationClipFile& clip, const svp::exec::CancellationToken& cancellation) {
  const svp::exec::worker::HostFacts host = svp::exec::worker::detect_host_facts();
  const CalibrationConditions wanted = track_window_calibration_conditions(setup, runtime_id, host);
  return stored_or_measure(std::string(kCoordinatorCalibrationName), wanted, [&] {
        const svp::exec::worker::BlobSource source = clip.blob();
        std::string cas_pattern =
            (std::filesystem::temp_directory_path() / "svp-track-calibration-cas-XXXXXX")
                .string();
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
                            std::string(svp::vision::tasks::kTrackWindowSourceRole));
        svp::exec::TaskTypeRegistry registry;
        svp::vision::tasks::register_track_window_task(
            registry, svp::vision::tasks::TrackWindowWorkerEnvironment{
                          .model_cache_root = setup.model_cache_root,
                          .ffmpeg_path = setup.ffmpeg_path,
                          .write_output =
                              [&artifacts](std::span<const std::byte> out, std::string media_type,
                                           std::string role) {
                                return artifacts.put(out, std::move(media_type), std::move(role));
                              },
                          .model_cache_for = {},
                          .record_start_failures = false});
        const svp::vision::tasks::TrackWindowTaskInputs inputs =
            calibration_inputs(setup, source.ref);
        const svp::exec::worker::AdmissionPolicy admission;
        const std::size_t max_slots = calibration::track_window_calibration_max_slots(
            inputs, svp::exec::worker::sample_memory().available_bytes,
            admission.reserve_bytes(host.physical_memory_bytes), host.logical_cpus);
        svp::exec::InProcessExecutor executor(
            registry, artifacts,
            svp::exec::InProcessExecutorOptions{.executor_id = "track-window-calibration",
                                                .threads = max_slots,
                                                .worker_session_id = "ws_track_calibration",
                                                .runtime_id = {}});
        return calibration::calibrate_track_window_capacity(executor, max_slots, inputs,
                                                            cancellation);
      });
}

CapacityOutcome ensure_worker_track_window_calibration(
    const svp::exec::remote::PairingKey& pairing, const WorkerSupplies& supplies,
    const svp::exec::worker::WorkerHelloAck& ack, const TrackWindowCalibrationSetup& setup,
    CalibrationClipFile& clip, const svp::exec::CancellationToken& cancellation) {
  const CalibrationConditions wanted = track_window_calibration_conditions(
      setup, svp::exec::blake3_prefixed(supplies.runtime.runtime_id), ack.host);
  return stored_or_measure(pairing.pairing_id, wanted, [&] {
        const svp::exec::worker::BlobSource source = clip.blob();
        auto with_clip = std::make_shared<WorkerSupplies>(supplies);
        with_clip->blobs = {source};
        const svp::vision::tasks::TrackWindowTaskInputs inputs =
            calibration_inputs(setup, source.ref);
        const std::size_t max_slots = calibration::track_window_calibration_max_slots(
            inputs, ack.memory.available_bytes, ack.memory_reserve_bytes, ack.host.logical_cpus);
        svp::exec::remote::RemoteExecutor executor(with_supplied_sessions(
            svp::exec::remote::RemoteExecutorOptions{.executor_id = "track-window-calibration." + pairing.pairing_id,
                                                     .connector = {.pairing = pairing},
                                                     .slots = max_slots},
            with_clip));
        return calibration::calibrate_track_window_capacity(executor, max_slots, inputs,
                                                            cancellation);
      });
}

std::string describe_track_window_calibration(const calibration::CapacityCalibration& capacity) {
  std::ostringstream out;
  out << capacity.slots << " tracking slot(s), " << capacity.seconds_per_item
      << " s per frame per slot (";
  for (std::size_t index = 0; index < capacity.sweep.size(); ++index) {
    const calibration::CapacityStep& step = capacity.sweep[index];
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%.2f", step.items_per_second);
    out << (index == 0 ? "" : ", ") << step.slots << " slot(s) " << rate << " frames/s";
  }
  out << "; " << capacity.stopped_because << ")";
  return out.str();
}

}  // namespace svp::builder::workers
