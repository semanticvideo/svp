#pragma once

// FLEET_MEMBER: a coordinator holding the fleet secret issuing a worker's
// member key (fleet_keys.hpp) over a proven pairing session, for a worker it
// already pairs whose HELLO_ACK `fleet` shows no member key (for example one
// whose join credential was migrated to a key-bound join id, which drops the
// old id's member key, fleet_store.hpp). It needs neither the worker's join
// listener nor a valid token: the session's pairing proof (pairing_proof.hpp)
// already authenticates both ends, and the join id comes from the worker over
// that session.
//
//   C -> W  FLEET_MEMBER {"join_id":"svpj-...","member_key":"<64 hex>"}
//   W -> C  FLEET_MEMBER {"message":"...","stored":bool}
//
// Sent only to a worker whose HELLO_ACK has `fleet` (older workers end a
// session on an unknown message), and served only on a session whose
// pairing proof verified.

#include "svp/exec/frame.hpp"
#include "svp/exec/worker/hello_messages.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker {

struct MemberKeyIssue {
  std::string join_id;
  std::vector<std::byte> member_key;
};

struct MemberKeyAnswer {
  bool stored = false;
  std::string message;
};

[[nodiscard]] Frame make_member_key_issue_frame(const MemberKeyIssue& issue);
// Throws ExecError for a body that is not an issue.
[[nodiscard]] MemberKeyIssue member_key_issue_from_frame(const Frame& frame);
[[nodiscard]] Frame make_member_key_answer_frame(const MemberKeyAnswer& answer);
[[nodiscard]] MemberKeyAnswer member_key_answer_from_frame(const Frame& frame);

// Whether a coordinator of `fleet_id` should issue the member key to the
// worker that answered `ack`.
[[nodiscard]] bool worker_needs_member_key(const WorkerHelloAck& ack, std::string_view fleet_id);

}  // namespace svp::exec::worker
