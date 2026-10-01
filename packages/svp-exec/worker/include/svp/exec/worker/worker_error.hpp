#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace svp::exec::worker {

enum class WorkerErrorCode {
  // A message broke the worker protocol (wrong type, order, or body).
  protocol,
  // The worker refused the session (HELLO_ACK accepted=false).
  refused,
  // Transferred bytes did not verify (size, BLAKE3, manifest, model lock).
  verification,
  // A file or directory could not be read or written.
  io,
  // Pairing material, a path, or an option is unusable.
  configuration,
  // An external command (ssh, launchctl, plutil) failed.
  command,
};

[[nodiscard]] std::string_view worker_error_code_name(WorkerErrorCode code) noexcept;

class WorkerError : public std::runtime_error {
 public:
  WorkerError(WorkerErrorCode code, std::string message);
  [[nodiscard]] WorkerErrorCode code() const noexcept;

 private:
  WorkerErrorCode code_;
};

}  // namespace svp::exec::worker
