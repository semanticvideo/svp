#pragma once

// Running the OCR capacity calibration (calibration/ocr_capacity_calibration
// .hpp) on this Mac and on paired workers, and keeping the results
// (calibration_store.hpp). A stored record is reused while its conditions
// hold; otherwise the Mac is measured again (after a runtime or macOS
// update, a hardware change, or different OCR thread counts or decoder).

#include "calibration_store.hpp"
#include "calibration/ocr_capacity_calibration.hpp"
#include "worker_supplies.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/pp_ocr.hpp"
#include "svp/vision/tasks/ocr_calibration_clip.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder::workers {

// The OCR configuration a calibration measures: a build's, or the default
// build's for `workers pair` / `workers sync`.
struct OcrCalibrationSetup {
  svp::vision::PpOcrOptions pp_ocr;
  std::vector<svp::exec::TaskModelRef> model_refs;
  std::filesystem::path ffmpeg_path;
  std::string ffmpeg_build;
};

// The thread plan a default build resolves on this Mac with `model_cache`.
[[nodiscard]] svp::models::ThreadPlan default_build_thread_plan(
    const std::filesystem::path& model_cache);

// The default build's OCR configuration with `model_cache` and the ffmpeg a
// default build resolves (runtime bundle, $SVP_FFMPEG, PATH). Throws
// std::runtime_error when ffmpeg or the PP-OCR bundles are missing.
[[nodiscard]] OcrCalibrationSetup default_ocr_calibration_setup(
    const std::filesystem::path& model_cache);

// A calibration clip, written on first use into a private temporary
// directory removed with this object. Thread-safe.
class CalibrationClipFile {
 public:
  // Writes the clip into the directory with ffmpeg and returns its path.
  using Writer = std::function<std::filesystem::path(const std::filesystem::path& ffmpeg,
                                                     const std::filesystem::path& directory)>;

  // The OCR calibration clip (ocr_calibration_clip.hpp).
  explicit CalibrationClipFile(std::filesystem::path ffmpeg);
  CalibrationClipFile(std::filesystem::path ffmpeg, Writer writer);
  ~CalibrationClipFile();
  CalibrationClipFile(const CalibrationClipFile&) = delete;
  CalibrationClipFile& operator=(const CalibrationClipFile&) = delete;

  [[nodiscard]] svp::exec::worker::BlobSource blob();

 private:
  std::filesystem::path ffmpeg_;
  Writer writer_;
  std::mutex mutex_;
  std::filesystem::path directory_;
  std::optional<svp::exec::worker::BlobSource> blob_;
};

[[nodiscard]] CalibrationConditions calibration_conditions(
    const OcrCalibrationSetup& setup, const std::string& runtime_id,
    const svp::exec::worker::HostFacts& host);

struct CalibrationOutcome {
  calibration::OcrCalibration ocr;
  // False when a stored record still held.
  bool measured = false;
};

// This Mac, in this process.
[[nodiscard]] CalibrationOutcome ensure_coordinator_calibration(
    const OcrCalibrationSetup& setup, const std::string& runtime_id, CalibrationClipFile& clip,
    const CalibrationStore& store, const svp::exec::CancellationToken& cancellation);

// A paired worker, over its own sessions: `supplies` (HELLO, runtime,
// models) plus the clip; `ack` is the worker's HELLO_ACK from this run.
[[nodiscard]] CalibrationOutcome ensure_worker_calibration(
    const svp::exec::remote::PairingKey& pairing, const WorkerSupplies& supplies,
    const svp::exec::worker::WorkerHelloAck& ack, const OcrCalibrationSetup& setup,
    CalibrationClipFile& clip, const CalibrationStore& store,
    const svp::exec::CancellationToken& cancellation);

[[nodiscard]] std::string describe_calibration(const calibration::OcrCalibration& ocr);

}  // namespace svp::builder::workers
