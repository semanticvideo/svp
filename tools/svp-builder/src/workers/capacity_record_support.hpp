#pragma once

// What every per-type capacity calibration run shares (the dispatched vision
// types, dispatched_calibration_runs.hpp; the audio types,
// audio_calibration_runs.hpp): reuse of a stored record while its conditions
// hold, and the private scratch a measurement on this Mac needs.

#include "calibration_store.hpp"
#include "dispatched_calibration_runs.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::workers {

// The stored record of `task_type` for `name` when its conditions are
// `wanted`; otherwise `measure()`, stored.
[[nodiscard]] CapacityOutcome stored_or_measured_capacity(
    const std::string& name, std::string_view task_type, const CalibrationConditions& wanted,
    const CalibrationStore& store,
    const std::function<calibration::CapacityCalibration()>& measure);

// A calibration clip's bytes. Throws std::runtime_error when it cannot be
// read.
[[nodiscard]] std::vector<std::byte> read_calibration_clip(const std::filesystem::path& path);

// A private temporary directory, removed with this object.
class CalibrationTemporaryDirectory {
 public:
  explicit CalibrationTemporaryDirectory(const std::string& stem);
  ~CalibrationTemporaryDirectory();
  CalibrationTemporaryDirectory(const CalibrationTemporaryDirectory&) = delete;
  CalibrationTemporaryDirectory& operator=(const CalibrationTemporaryDirectory&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace svp::builder::workers
