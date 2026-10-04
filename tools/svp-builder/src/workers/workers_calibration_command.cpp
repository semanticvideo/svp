#include "workers_calibration_command.hpp"

#include "audio_calibration_runs.hpp"
#include "default_build_calibration.hpp"
#include "dispatched_calibration_runs.hpp"
#include "ocr_calibration_runs.hpp"
#include "track_window_calibration_runs.hpp"
#include "worker_restart.hpp"
#include "worker_supplies.hpp"

#include "calibration/audio_capacity_workloads.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/worker/model_bundles.hpp"
#include "svp/exec/worker/worker_connection.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace svp::builder::workers {
namespace {

// The clips the measurements run on, each written on first use: OCR's (also
// the vision types'), the speech clip, and one per tracking cadence.
class CalibrationClips {
 public:
  explicit CalibrationClips(const DefaultBuildCalibration& work)
      : work_(work),
        ocr_(work.ocr.ffmpeg_path),
        speech_(work.ocr.ffmpeg_path,
                [](const std::filesystem::path& ffmpeg, const std::filesystem::path& directory) {
                  return calibration::write_speech_calibration_clip(ffmpeg, directory);
                }) {}

  CalibrationClipFile& ocr() { return ocr_; }
  CalibrationClipFile& speech() { return speech_; }
  CalibrationClipFile& tracking(svp::vision::VisualTrackingQuality quality) {
    std::unique_ptr<CalibrationClipFile>& clip = tracking_[quality];
    if (!clip) {
      clip = track_window_calibration_clip_file(work_.tracking.at(quality));
    }
    return *clip;
  }

 private:
  const DefaultBuildCalibration& work_;
  CalibrationClipFile ocr_;
  CalibrationClipFile speech_;
  std::map<svp::vision::VisualTrackingQuality, std::unique_ptr<CalibrationClipFile>> tracking_;
};

std::string step_name(const calibration::CalibrationStep& step) {
  if (step.kind == calibration::CalibrationKind::tracking) {
    return step.task_type + " (" +
           std::string(svp::vision::visual_tracking_quality_name(step.tracking_quality)) +
           " tracking)";
  }
  return step.task_type;
}

std::string describe(const calibration::CalibrationStep& step,
                     const calibration::CapacityCalibration& capacity) {
  if (step.kind == calibration::CalibrationKind::tracking) {
    return std::string(svp::vision::visual_tracking_quality_name(step.tracking_quality)) +
           " tracking: " + describe_track_window_calibration(capacity);
  }
  return describe_capacity(step.task_type, capacity);
}

void print(const char* mac, const calibration::CalibrationStep& step,
           const CapacityOutcome& outcome) {
  std::cout << mac << (outcome.measured ? "calibrated: " : "calibration current: ")
            << describe(step, outcome.capacity) << "\n";
}

}  // namespace

bool calibrate_for_workers_command(const svp::exec::worker::CoordinatorPairingRecord& record,
                                   const svp::exec::worker::CoordinatorHello& hello,
                                   const svp::exec::worker::CoordinatorRuntime& runtime,
                                   const std::filesystem::path& model_cache) {
  try {
    const DefaultBuildCalibration work = default_build_calibration(model_cache);
    for (const std::string& skipped : work.skipped) {
      std::cerr << "svp-builder: warning: not calibrated: " << skipped << "\n";
    }
    WorkerSupplies supplies;
    supplies.hello = hello;
    supplies.runtime = runtime;
    supplies.models = svp::exec::worker::prepare_model_bundles(model_cache, work.model_ids);
    CalibrationClips clips(work);
    const CalibrationStore store;
    const svp::exec::CancellationToken never_cancelled;
    const std::string runtime_id = svp::exec::blake3_prefixed(runtime.runtime_id);

    const CalibrationOutcome local =
        ensure_coordinator_calibration(work.ocr, runtime_id, clips.ocr(), store, never_cancelled);
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
        record.key, supplies, ack, work.ocr, clips.ocr(), store, never_cancelled);
    std::cout << "worker: " << (worker.measured ? "calibrated: " : "calibration current: ")
              << describe_calibration(worker.ocr) << "\n";

    // Every other type, this Mac first, then the worker, in the plan's order
    // (diarize.window last: this Mac's measurement loads sherpa-onnx). They
    // are measured best effort: the command's outcome stays OCR's, and a
    // --distributed build measures any type missing here.
    for (const calibration::CalibrationStep& step : work.steps) {
      if (step.kind == calibration::CalibrationKind::ocr) {
        continue;
      }
      try {
        CapacityOutcome mine;
        CapacityOutcome theirs;
        switch (step.kind) {
          case calibration::CalibrationKind::dispatched:
            mine = ensure_coordinator_capacity(step.task_type, work.dispatched, runtime_id,
                                               clips.ocr(), store, model_cache,
                                               work.ocr.ffmpeg_path, never_cancelled);
            print("this Mac: ", step, mine);
            theirs = ensure_worker_capacity(record.key, supplies, ack, step.task_type,
                                            work.dispatched, clips.ocr(), work.ocr.ffmpeg_path,
                                            store, never_cancelled);
            break;
          case calibration::CalibrationKind::tracking: {
            const TrackWindowCalibrationSetup& setup = work.tracking.at(step.tracking_quality);
            CalibrationClipFile& clip = clips.tracking(step.tracking_quality);
            mine = ensure_coordinator_track_window_calibration(setup, runtime_id, clip,
                                                               never_cancelled);
            print("this Mac: ", step, mine);
            theirs = ensure_worker_track_window_calibration(record.key, supplies, ack, setup, clip,
                                                            never_cancelled);
            break;
          }
          case calibration::CalibrationKind::audio:
            mine = ensure_coordinator_audio_capacity(step.task_type, work.audio, runtime_id,
                                                     clips.speech(), store, model_cache,
                                                     never_cancelled);
            print("this Mac: ", step, mine);
            theirs = ensure_worker_audio_capacity(record.key, supplies, ack, step.task_type,
                                                  work.audio, clips.speech(), store,
                                                  never_cancelled);
            break;
          case calibration::CalibrationKind::ocr:
            break;
        }
        print("worker: ", step, theirs);
      } catch (const std::exception& error) {
        std::cerr << "svp-builder: warning: " << step_name(step)
                  << " capacity calibration failed: " << error.what()
                  << "\n  a --distributed build measures it again\n";
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
