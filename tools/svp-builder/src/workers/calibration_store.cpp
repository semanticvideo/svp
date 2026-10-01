#include "calibration_store.hpp"

#include "svp/exec/canonical_json.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace svp::builder::workers {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

namespace {

nlohmann::json conditions_json(const CalibrationConditions& conditions) {
  return {{"cpu_brand", conditions.cpu_brand},
          {"logical_cpus", conditions.logical_cpus},
          {"model_bundles", conditions.model_bundles},
          {"os", {{"build", conditions.os.build},
                  {"product_version", conditions.os.product_version}}},
          {"parameters_blake3", conditions.parameters_blake3},
          {"physical_memory_bytes", conditions.physical_memory_bytes},
          {"runtime_id", conditions.runtime_id}};
}

CalibrationConditions conditions_from(const nlohmann::json& value) {
  return CalibrationConditions{
      .runtime_id = value.at("runtime_id").get<std::string>(),
      .os = {.product_version = value.at("os").at("product_version").get<std::string>(),
             .build = value.at("os").at("build").get<std::string>()},
      .cpu_brand = value.at("cpu_brand").get<std::string>(),
      .logical_cpus = value.at("logical_cpus").get<std::uint32_t>(),
      .physical_memory_bytes = value.at("physical_memory_bytes").get<std::uint64_t>(),
      .parameters_blake3 = value.at("parameters_blake3").get<std::string>(),
      .model_bundles = value.at("model_bundles").get<std::string>()};
}

}  // namespace

std::filesystem::path default_calibration_dir() {
  return svp::exec::worker::default_coordinator_pairings_dir().parent_path() / "Calibration";
}

CalibrationStore::CalibrationStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::optional<CalibrationRecord> CalibrationStore::read(const std::string& name) const {
  std::ifstream file(directory_ / (name + ".json"), std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  try {
    std::ostringstream text;
    text << file.rdbuf();
    const nlohmann::json value = nlohmann::json::parse(text.str());
    if (value.at("schema").get<std::string>() != kCalibrationRecordSchema) {
      return std::nullopt;
    }
    return CalibrationRecord{
        .conditions = conditions_from(value.at("conditions")),
        .ocr = calibration::ocr_calibration_from_json(value.at("ocr_frame_batch")),
        .measured_at = value.at("measured_at").get<std::string>()};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void CalibrationStore::write(const std::string& name, const CalibrationRecord& record) const {
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  ::chmod(directory_.c_str(), 0700);
  const nlohmann::json value{{"conditions", conditions_json(record.conditions)},
                             {"measured_at", record.measured_at},
                             {"ocr_frame_batch", calibration::ocr_calibration_to_json(record.ocr)},
                             {"schema", std::string(kCalibrationRecordSchema)}};
  const std::filesystem::path target = directory_ / (name + ".json");
  const std::filesystem::path temporary =
      directory_ / (name + ".json.tmp-" + std::to_string(::getpid()));
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << value.dump(2) << "\n";
    if (!file) {
      throw WorkerError(WorkerErrorCode::io, "cannot write " + temporary.string());
    }
  }
  ::chmod(temporary.c_str(), 0600);
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    throw WorkerError(WorkerErrorCode::io, "cannot write " + target.string());
  }
}

void CalibrationStore::remove(const std::string& name) const {
  std::error_code error;
  std::filesystem::remove(directory_ / (name + ".json"), error);
  // The directory goes once its last record does.
  if (std::filesystem::is_empty(directory_, error) && !error) {
    std::filesystem::remove(directory_, error);
  }
}

}  // namespace svp::builder::workers
