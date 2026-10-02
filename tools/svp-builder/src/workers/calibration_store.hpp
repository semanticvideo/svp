#pragma once

// Where this coordinator keeps OCR capacity measurements (plan §3.5): one
// record for this Mac and one per paired worker, beside the pairing records:
//
//   <SVP support>/Calibration/coordinator.json
//   <SVP support>/Calibration/<pairing_id>.json
//
// (<SVP support> is the parent of the pairings directory, so SVP_PAIRINGS_DIR
// relocates both.) A record is valid only for the exact conditions it was
// measured under: the runtime, the macOS version and build, the hardware
// (CPU and memory), and the calibration batch's parameters (thread counts,
// decoder build, calibration slice) and model bundles. Anything else changed
// means it is measured again.

#include "calibration/ocr_capacity_calibration.hpp"

#include "svp/exec/worker/host_facts.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace svp::builder::workers {

inline constexpr std::string_view kCalibrationRecordSchema = "svp.ocr-calibration/1";
inline constexpr std::string_view kCoordinatorCalibrationName = "coordinator";

// The conditions a calibration holds for.
struct CalibrationConditions {
  std::string runtime_id;
  svp::exec::worker::OsIdentity os;
  std::string cpu_brand;
  std::uint32_t logical_cpus = 0;
  std::uint64_t physical_memory_bytes = 0;
  // ocr_calibration_spec(...): its parameters_blake3, and its model bundles.
  std::string parameters_blake3;
  std::string model_bundles;

  bool operator==(const CalibrationConditions&) const = default;
};

struct CalibrationRecord {
  CalibrationConditions conditions;
  calibration::OcrCalibration ocr;
  std::string measured_at;
};

[[nodiscard]] std::filesystem::path default_calibration_dir();

class CalibrationStore {
 public:
  explicit CalibrationStore(std::filesystem::path directory = default_calibration_dir());

  // nullopt when absent or unreadable (a broken record is measured again).
  [[nodiscard]] std::optional<CalibrationRecord> read(const std::string& name) const;
  // Writes atomically (0600 file in a 0700 directory). Throws WorkerError(io).
  void write(const std::string& name, const CalibrationRecord& record) const;
  void remove(const std::string& name) const;

 private:
  std::filesystem::path directory_;
};

}  // namespace svp::builder::workers
