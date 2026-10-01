#include "svp/exec/remote/remote_error.hpp"

namespace svp::exec::remote {

std::string_view remote_error_code_name(RemoteErrorCode code) noexcept {
  switch (code) {
    case RemoteErrorCode::invalid_configuration:
      return "invalid_configuration";
    case RemoteErrorCode::listener_failed:
      return "listener_failed";
    case RemoteErrorCode::worker_not_found:
      return "worker_not_found";
    case RemoteErrorCode::no_route:
      return "no_route";
    case RemoteErrorCode::authentication_failed:
      return "authentication_failed";
    case RemoteErrorCode::cancelled:
      return "cancelled";
  }
  return "unknown";
}

RemoteTransportError::RemoteTransportError(RemoteErrorCode code, std::string message)
    : std::runtime_error(std::string(remote_error_code_name(code)) + ": " + message),
      code_(code) {}

RemoteErrorCode RemoteTransportError::code() const noexcept { return code_; }

}  // namespace svp::exec::remote
