#include "svp/exec/lease_frames.hpp"

#include "json_fields.hpp"
#include "record_identifiers.hpp"
#include "svp/exec/exec_error.hpp"

#include <limits>
#include <utility>

namespace svp::exec {
namespace {

constexpr std::string_view kLeaseMember = "lease";
constexpr std::string_view kTaskSpecMember = "task_spec";
constexpr std::string_view kLeaseIdMember = "lease_id";

void require_control_frame(const Frame& frame, MessageType expected) {
  if (frame.type != expected) {
    throw ExecError(ExecErrorCode::frame_malformed,
                    "expected a " + std::string(message_type_name(expected)) +
                        " frame, got " + std::string(message_type_name(frame.type)));
  }
  if (!frame.payloads.empty()) {
    throw ExecError(ExecErrorCode::frame_malformed,
                    std::string(message_type_name(expected)) +
                        " frames carry no payloads");
  }
}

std::string checked_lease_id(std::string lease_id) {
  if (!detail::is_record_identifier(lease_id)) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "lease_id must be a non-empty [A-Za-z0-9._-] identifier: `" +
                        lease_id + "`");
  }
  return lease_id;
}

std::uint64_t positive_ms(std::chrono::milliseconds value, std::string_view name) {
  if (value.count() <= 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "lease." + std::string(name) + " must be at least 1 ms");
  }
  return static_cast<std::uint64_t>(value.count());
}

std::uint64_t positive_field(const nlohmann::json& object, std::string_view name,
                             std::string_view path) {
  const std::uint64_t value = detail::required_unsigned(object, name, path);
  if (value == 0) {
    throw ExecError(ExecErrorCode::invalid_value,
                    std::string(path) + "." + std::string(name) + " must be at least 1");
  }
  return value;
}

std::chrono::milliseconds positive_ms_field(const nlohmann::json& object,
                                            std::string_view name, std::string_view path) {
  using Rep = std::chrono::milliseconds::rep;
  const std::uint64_t value = positive_field(object, name, path);
  if (value > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
    throw ExecError(ExecErrorCode::invalid_value,
                    std::string(path) + "." + std::string(name) + " is out of range");
  }
  return std::chrono::milliseconds(static_cast<Rep>(value));
}

Frame lease_id_frame(MessageType type, std::string_view lease_id) {
  return Frame{.type = type,
               .body = nlohmann::json{{kLeaseIdMember, checked_lease_id(std::string(lease_id))}},
               .payloads = {}};
}

std::string lease_id_from(const Frame& frame, MessageType type) {
  require_control_frame(frame, type);
  const std::string path = std::string(message_type_name(type)) + ".body";
  detail::require_object(frame.body, path);
  detail::reject_unknown_fields(frame.body, {kLeaseIdMember}, path);
  return checked_lease_id(detail::required_string(frame.body, kLeaseIdMember, path));
}

}  // namespace

Frame make_leased_assign_frame(const TaskSpec& spec, const Lease& lease) {
  if (lease.attempt == 0) {
    throw ExecError(ExecErrorCode::invalid_value, "lease.attempt must be at least 1");
  }
  nlohmann::json lease_json{
      {"attempt", lease.attempt},
      {"expires_in_ms", positive_ms(lease.duration, "expires_in_ms")},
      {"heartbeat_interval_ms",
       positive_ms(lease.heartbeat_interval, "heartbeat_interval_ms")},
      {"lease_id", checked_lease_id(lease.lease_id)}};
  return Frame{.type = MessageType::assign,
               .body = nlohmann::json{{kLeaseMember, std::move(lease_json)},
                                      {kTaskSpecMember, task_spec_to_json(spec)}},
               .payloads = {}};
}

LeasedAssignment leased_assignment_from_frame(const Frame& frame) {
  require_control_frame(frame, MessageType::assign);
  constexpr std::string_view kBody = "ASSIGN.body";
  constexpr std::string_view kLease = "ASSIGN.body.lease";
  detail::require_object(frame.body, kBody);
  detail::reject_unknown_fields(frame.body, {kLeaseMember, kTaskSpecMember}, kBody);
  const nlohmann::json& lease_json = detail::required_object(frame.body, kLeaseMember, kBody);
  detail::reject_unknown_fields(
      lease_json, {"attempt", "expires_in_ms", "heartbeat_interval_ms", "lease_id"},
      kLease);

  Lease lease;
  lease.lease_id = checked_lease_id(detail::required_string(lease_json, "lease_id", kLease));
  lease.attempt = positive_field(lease_json, "attempt", kLease);
  lease.duration = positive_ms_field(lease_json, "expires_in_ms", kLease);
  lease.heartbeat_interval = positive_ms_field(lease_json, "heartbeat_interval_ms", kLease);
  return LeasedAssignment{
      .spec = task_spec_from_json(
          detail::required_object(frame.body, kTaskSpecMember, kBody)),
      .lease = std::move(lease)};
}

Frame make_heartbeat_frame(std::string_view lease_id) {
  return lease_id_frame(MessageType::heartbeat, lease_id);
}

std::string lease_id_from_heartbeat_frame(const Frame& frame) {
  return lease_id_from(frame, MessageType::heartbeat);
}

Frame make_cancel_frame(std::string_view lease_id) {
  return lease_id_frame(MessageType::cancel, lease_id);
}

std::string lease_id_from_cancel_frame(const Frame& frame) {
  return lease_id_from(frame, MessageType::cancel);
}

Frame make_shutdown_frame() {
  return Frame{.type = MessageType::shutdown, .body = nlohmann::json::object(), .payloads = {}};
}

}  // namespace svp::exec
