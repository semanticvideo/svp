#include "svp/exec/worker/worker_error.hpp"

#include <utility>

namespace svp::exec::worker {

std::string_view worker_error_code_name(WorkerErrorCode code) noexcept {
  switch (code) {
    case WorkerErrorCode::protocol:
      return "protocol";
    case WorkerErrorCode::refused:
      return "refused";
    case WorkerErrorCode::verification:
      return "verification";
    case WorkerErrorCode::io:
      return "io";
    case WorkerErrorCode::configuration:
      return "configuration";
    case WorkerErrorCode::command:
      return "command";
  }
  return "unknown";
}

WorkerError::WorkerError(WorkerErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

WorkerErrorCode WorkerError::code() const noexcept { return code_; }

}  // namespace svp::exec::worker
