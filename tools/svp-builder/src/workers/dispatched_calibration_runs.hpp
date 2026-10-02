#pragma once

// Capacity calibration of the dispatched vision task types (M4;
// calibration/dispatched_capacity_workloads.hpp) on this Mac and on paired
// workers, kept in the calibration store like OCR's (calibration_store.hpp).
// A stored record is reused while its conditions hold: the runtime, macOS,
// hardware, and the type's settings (model bundle, thread counts, decoder,
// workload recipe). Otherwise the Mac is measured again.

#include "calibration_store.hpp"
#include "ocr_calibration_runs.hpp"
#include "worker_supplies.hpp"

#include "calibration/capacity_sweep.hpp"
#include "calibration/dispatched_capacity_workloads.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/remote/pairing_key.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace svp::builder::workers {

struct CapacityOutcome {
  calibration::CapacityCalibration capacity;
  // False when a stored record still held.
  bool measured = false;
};

// This Mac, in this process, with its own model cache and ffmpeg.
[[nodiscard]] CapacityOutcome ensure_coordinator_capacity(
    std::string_view task_type, const calibration::DispatchedCalibrationSetup& setup,
    const std::string& runtime_id, CalibrationClipFile& clip, const CalibrationStore& store,
    const std::filesystem::path& model_cache, const std::filesystem::path& ffmpeg,
    const svp::exec::CancellationToken& cancellation);

// A paired worker, over its own sessions: `supplies` (HELLO, runtime,
// models) plus the clip; `ack` is the worker's HELLO_ACK from this run.
// `ffmpeg` is this Mac's (the depth workload names the clip's pixels).
[[nodiscard]] CapacityOutcome ensure_worker_capacity(
    const svp::exec::remote::PairingKey& pairing, const WorkerSupplies& supplies,
    const svp::exec::worker::WorkerHelloAck& ack, std::string_view task_type,
    const calibration::DispatchedCalibrationSetup& setup, CalibrationClipFile& clip,
    const std::filesystem::path& ffmpeg, const CalibrationStore& store,
    const svp::exec::CancellationToken& cancellation);

[[nodiscard]] std::string describe_capacity(std::string_view task_type,
                                            const calibration::CapacityCalibration& capacity);

}  // namespace svp::builder::workers
