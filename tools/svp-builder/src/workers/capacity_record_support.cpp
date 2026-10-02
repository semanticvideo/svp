#include "capacity_record_support.hpp"

#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unistd.h>

namespace svp::builder::workers {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

CapacityOutcome stored_or_measured_capacity(
    const std::string& name, std::string_view task_type, const CalibrationConditions& wanted,
    const CalibrationStore& store,
    const std::function<calibration::CapacityCalibration()>& measure) {
  if (const std::optional<CapacityRecord> record = store.read_capacity(name, task_type);
      record && record->conditions == wanted) {
    return CapacityOutcome{.capacity = record->capacity, .measured = false};
  }
  CapacityRecord record{.conditions = wanted,
                        .capacity = measure(),
                        .measured_at = svp::exec::worker::utc_timestamp_now()};
  store.write_capacity(name, task_type, record);
  return CapacityOutcome{.capacity = record.capacity, .measured = true};
}

std::vector<std::byte> read_calibration_clip(const std::filesystem::path& path) {
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

CalibrationTemporaryDirectory::CalibrationTemporaryDirectory(const std::string& stem) {
  std::string pattern = (std::filesystem::temp_directory_path() / (stem + "-XXXXXX")).string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw WorkerError(WorkerErrorCode::io, "cannot create a calibration directory");
  }
  path_ = pattern;
}

CalibrationTemporaryDirectory::~CalibrationTemporaryDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

}  // namespace svp::builder::workers
