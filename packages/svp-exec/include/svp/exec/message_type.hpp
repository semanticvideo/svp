#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace svp::exec {

// Frame message types (plan §4.3). The wire name is the upper-case spelling
// returned by message_type_name(). Query/answer pairs the plan writes as
// `RUNTIME_HAVE?` / `BLOB_HAVE?` share one type each; the direction is carried
// by the message body, which later protocol work defines.
enum class MessageType {
  hello,
  hello_ack,
  runtime_have,
  runtime_put,
  blob_have,
  blob_put,
  calibrate,
  assign,
  accept,
  reject,
  heartbeat,
  result,
  cancel,
  drain,
  shutdown,
  unpair,
  error,
  // Worker protocol 1.1 (M6): a coordinator fetching a blob from the
  // worker's CAS (worker/transfer_messages.hpp).
  blob_get,
  // Worker protocol 1.2: a coordinator releasing blobs it no longer needs on
  // the worker (worker/transfer_messages.hpp).
  blob_release,
  // Fleet join (worker/fleet_join.hpp), spoken only on a worker's join
  // listener, never in a coordinator session.
  join_offer,
  join_challenge,
  join_accept,
  join_done,
  // Fleet membership over a proven pairing session (worker/fleet_join.hpp):
  // a coordinator issuing a worker's member key, and the worker's answer.
  fleet_member,
};

inline constexpr std::array kAllMessageTypes = {
    MessageType::hello,     MessageType::hello_ack, MessageType::runtime_have,
    MessageType::runtime_put, MessageType::blob_have, MessageType::blob_put,
    MessageType::calibrate, MessageType::assign,    MessageType::accept,
    MessageType::reject,    MessageType::heartbeat, MessageType::result,
    MessageType::cancel,    MessageType::drain,     MessageType::shutdown,
    MessageType::unpair,    MessageType::error,     MessageType::blob_get,
    MessageType::blob_release, MessageType::join_offer, MessageType::join_challenge,
    MessageType::join_accept, MessageType::join_done, MessageType::fleet_member,
};

[[nodiscard]] std::string_view message_type_name(MessageType type) noexcept;

// Exact, case-sensitive match of a wire name; nullopt for anything else.
[[nodiscard]] std::optional<MessageType> parse_message_type(
    std::string_view name) noexcept;

}  // namespace svp::exec
