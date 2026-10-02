#pragma once

// Running the tracking capacity calibration
// (calibration/track_window_capacity_calibration.hpp) on this Mac and on
// paired workers, and keeping the results as capacity records in the
// calibration store (calibration_store.hpp). Each Mac keeps one record per
// tracking workload, of task type "track.window.<workload>" where the
// workload is a digest of the calibration
// window's parameters (the quality's cadence and depth schedule, thread
// counts, detector settings, decoder) and recipe, so a build at another
// quality measures once and later builds at either quality reuse their own
// record. A stored record is reused while its conditions hold;
// otherwise the Mac is measured again (after a runtime or macOS update, a
// hardware change, or different tracking options, thread counts, decoder, or
// model bundles).

#include "calibration_store.hpp"
#include "dispatched_calibration_runs.hpp"
#include "ocr_calibration_runs.hpp"
#include "worker_supplies.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace svp::builder::workers {

// The tracking configuration a calibration measures: a build's.
struct TrackWindowCalibrationSetup {
  svp::vision::VisualEntityPipelineOptions options;
  std::vector<svp::exec::TaskModelRef> model_refs;
  std::filesystem::path model_cache_root;
  std::filesystem::path ffmpeg_path;
  std::string ffmpeg_build;
};

// The calibration clip for `setup`'s cadence, written on first use.
[[nodiscard]] std::unique_ptr<CalibrationClipFile> track_window_calibration_clip_file(
    const TrackWindowCalibrationSetup& setup);

[[nodiscard]] CalibrationConditions track_window_calibration_conditions(
    const TrackWindowCalibrationSetup& setup, const std::string& runtime_id,
    const svp::exec::worker::HostFacts& host);

// This Mac, in this process.
[[nodiscard]] CapacityOutcome ensure_coordinator_track_window_calibration(
    const TrackWindowCalibrationSetup& setup, const std::string& runtime_id,
    CalibrationClipFile& clip, const svp::exec::CancellationToken& cancellation);

// A paired worker, over its own sessions: `supplies` (HELLO, runtime,
// models) plus the clip; `ack` is the worker's HELLO_ACK from this run.
[[nodiscard]] CapacityOutcome ensure_worker_track_window_calibration(
    const svp::exec::remote::PairingKey& pairing, const WorkerSupplies& supplies,
    const svp::exec::worker::WorkerHelloAck& ack, const TrackWindowCalibrationSetup& setup,
    CalibrationClipFile& clip, const svp::exec::CancellationToken& cancellation);

[[nodiscard]] std::string describe_track_window_calibration(
    const calibration::CapacityCalibration& capacity);

}  // namespace svp::builder::workers
