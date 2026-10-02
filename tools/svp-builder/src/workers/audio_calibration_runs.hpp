#pragma once

// Capacity calibration of the dispatched audio task types (M5;
// calibration/audio_capacity_workloads.hpp) on this Mac and on paired
// workers, kept in the calibration store like the vision types'
// (calibration_store.hpp). A stored record is reused while its conditions
// hold: the runtime, macOS, hardware, and the type's settings (model
// bundles, thread counts, sherpa-onnx library, workload recipe, clip).
//
// This Mac measures diarize.window only in its diarization stage
// (DistributedFleet::measure_in_stage): the measurement loads sherpa-onnx,
// which must not load before the build's own ONNX Runtime models.

#include "calibration_store.hpp"
#include "dispatched_calibration_runs.hpp"
#include "ocr_calibration_runs.hpp"
#include "worker_supplies.hpp"

#include "calibration/audio_capacity_workloads.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/remote/pairing_key.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace svp::builder::workers {

// This Mac, in this process, with its own model cache. An asr.chunk_batch
// measurement releases the Whisper and aligner models it loaded.
[[nodiscard]] CapacityOutcome ensure_coordinator_audio_capacity(
    std::string_view task_type, const calibration::AudioCalibrationSetup& setup,
    const std::string& runtime_id, CalibrationClipFile& clip, const CalibrationStore& store,
    const std::filesystem::path& model_cache,
    const svp::exec::CancellationToken& cancellation);

// A paired worker, over its own sessions: `supplies` plus the clip.
[[nodiscard]] CapacityOutcome ensure_worker_audio_capacity(
    const svp::exec::remote::PairingKey& pairing, const WorkerSupplies& supplies,
    const svp::exec::worker::WorkerHelloAck& ack, std::string_view task_type,
    const calibration::AudioCalibrationSetup& setup, CalibrationClipFile& clip,
    const CalibrationStore& store, const svp::exec::CancellationToken& cancellation);

}  // namespace svp::builder::workers
