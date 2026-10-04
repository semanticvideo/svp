#include "svp/exec/worker/fleet_member_messages.hpp"

#include "hex_bytes.hpp"
#include "message_fields.hpp"
#include "svp/exec/worker/fleet_keys.hpp"

namespace svp::exec::worker {

using namespace detail;

Frame make_member_key_issue_frame(const MemberKeyIssue& issue) {
  return Frame{.type = MessageType::fleet_member,
               .body = nlohmann::json{{"join_id", issue.join_id},
                                      {"member_key", bytes_hex(issue.member_key)}},
               .payloads = {}};
}

MemberKeyIssue member_key_issue_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::fleet_member, 0);
  reject_unknown_fields(frame.body, {"join_id", "member_key"}, path);
  const std::optional<std::vector<std::byte>> key =
      parse_hex_bytes(required_string(frame.body, "member_key", path), kFleetKeyBytes);
  if (!key) {
    throw ExecError(ExecErrorCode::invalid_value, path + ".member_key must be 64 lowercase hex");
  }
  return MemberKeyIssue{.join_id = required_string(frame.body, "join_id", path),
                        .member_key = *key};
}

Frame make_member_key_answer_frame(const MemberKeyAnswer& answer) {
  return Frame{.type = MessageType::fleet_member,
               .body = nlohmann::json{{"message", answer.message}, {"stored", answer.stored}},
               .payloads = {}};
}

MemberKeyAnswer member_key_answer_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::fleet_member, 0);
  return MemberKeyAnswer{.stored = required_bool(frame.body, "stored", path),
                         .message = required_string(frame.body, "message", path)};
}

bool worker_needs_member_key(const WorkerHelloAck& ack, std::string_view fleet_id) {
  return ack.fleet && !ack.fleet->member && ack.fleet->fleet_id == fleet_id &&
         is_fleet_identifier(ack.fleet->join_id, kWorkerJoinIdPrefix);
}

}  // namespace svp::exec::worker
