#pragma once

// Fleet join: pairing a worker with a coordinator over the SVP transport,
// without SSH and without a password, for any number of Macs and
// coordinators.
//
// A worker installed with `svp-builder worker install --join <worker token>`
// (fleet_keys.hpp) runs, besides one listener per pairing (worker_agent.hpp),
// a JOIN LISTENER: TLS 1.2 PSK with identity worker_join_id and key the
// token's join key, or from its first pairing on its member key
// (fleet_store.hpp join_listener_key), advertised in Bonjour with
//   pairing=<worker_join_id>  fleet=<fleet_id>  join=<join TXT>
// where the join TXT is "member", or "token.<token_id>.<expires_at>" before
// the first pairing. Only Macs holding the token, or a coordinator holding
// the fleet secret, complete TLS with it; a coordinator derives the key from
// the TXT entry (fleet_keys.hpp), and refuses a token past its expiry.
//
// Inside that connection, frames (frame.hpp):
//   C -> W  JOIN_OFFER     {"coordinator_ephemeral":"<130 hex>",
//                           "coordinator_id","fleet_id"}
//   W -> C  JOIN_CHALLENGE {"host":{HostFacts},"worker":{endpoint, with its
//                           worker_id (worker_identity.hpp)},
//                           "worker_ephemeral":"<130 hex>","worker_join_id"}
//   C -> W  JOIN_ACCEPT    {"member_key_wrapped":"<64 hex>","pairing_id",
//                           "signature":"<hex DER>"}
//   W -> C  JOIN_DONE      {"confirm":"<64 hex>"}
// or ERROR {"code","message"} from either side, which ends it.
//
// Both ephemerals are fresh P-256 keys (fleet_crypto.hpp). With
//   T  = canonical JSON {"coordinator_ephemeral","coordinator_id","fleet_id",
//        "pairing_id","worker_ephemeral","worker_join_id"}
//   th = BLAKE3(T)
//   S  = derive_key(kJoinPairingSecretContext, ECDH || th)   the pair's PSK
//   W  = member key XOR derive_key(kJoinMemberWrapContext, S)
// the coordinator signs kJoinSignaturePrefix + hex(th) + "\n" + hex(W) with
// the fleet signing key. The worker checks that pairing_id is
// fleet_pairing_id(coordinator_id, worker_join_id), verifies the signature
// with the fleet public key from its token, stores the pairing (S) and the
// member key, and answers confirm = keyed_hash(S, kJoinConfirmPrefix +
// hex(th)), which the coordinator checks before it records the pairing.
//
// What each party can do:
//   - Without the token or fleet secret: nothing (TLS fails).
//   - Another worker (holds a token or its own member key): cannot sign, so
//     cannot pair anyone or pose as a coordinator; cannot learn S from the
//     traffic (ECDH), even though TLS-PSK with a shared token key is not
//     forward secret; can at most pose as a new worker with its own token.
//   - A coordinator with the fleet secret: pairs any worker of the fleet.
// After the join, the coordinator and worker hold their own pairing (S) and
// meet exactly as SSH-paired ones do.

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/pairing_store.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker {

inline constexpr std::string_view kFleetTxtKey = "fleet";
inline constexpr std::string_view kJoinTxtKey = "join";
inline constexpr std::string_view kJoinTxtMember = "member";
inline constexpr std::string_view kJoinTxtTokenPrefix = "token.";

inline constexpr std::string_view kJoinPairingSecretContext = "svp fleet join pairing secret v1";
inline constexpr std::string_view kJoinMemberWrapContext = "svp fleet join member wrap v1";
inline constexpr std::string_view kJoinSignaturePrefix = "svp fleet join transcript v1\n";
inline constexpr std::string_view kJoinConfirmPrefix = "svp fleet join confirm v1\n";

// The join TXT value for a credential.
[[nodiscard]] std::string join_txt_value(const WorkerJoinCredential& credential);

struct JoinListenerKey {
  std::optional<svp::exec::remote::PairingKey> key;
  // Why there is none (expired or malformed token), for reports.
  std::string refusal;
};

// The PSK a coordinator holding `fleet` uses for the join listener of
// `worker_join_id` advertising `join_txt`, at UTC second `now`.
[[nodiscard]] JoinListenerKey coordinator_join_key(const FleetSecret& fleet,
                                                   std::string_view worker_join_id,
                                                   std::string_view join_txt, std::uint64_t now);

struct WorkerJoinResult {
  WorkerPairingRecord pairing;
  std::vector<std::byte> member_key;
};

// Worker side: serves one join. `self` describes this worker for the
// coordinator's record. `persist` stores the result (pairing record, member
// key) before JOIN_DONE is sent. Throws WorkerError (protocol, verification,
// refused) after sending ERROR when it can.
WorkerJoinResult serve_fleet_join(FrameReader& input, FrameWriter& output,
                                  const WorkerJoinCredential& credential,
                                  const WorkerEndpoint& self, const HostFacts& host,
                                  const std::function<void(const WorkerJoinResult&)>& persist);

struct CoordinatorJoinResult {
  svp::exec::remote::PairingKey key;
  WorkerEndpoint worker;
  HostFacts host;
};

// Coordinator side: pairs the worker `worker_join_id`. Throws WorkerError
// (protocol, verification, refused).
CoordinatorJoinResult join_fleet_worker(FrameReader& input, FrameWriter& output,
                                        const FleetMembership& membership,
                                        std::string_view worker_join_id);

}  // namespace svp::exec::worker
