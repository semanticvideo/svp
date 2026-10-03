#pragma once

// Fleet secrets, the keys derived from them, and join tokens (fleet_join.hpp
// describes the protocol that uses them).
//
// A fleet is the set of coordinators allowed to pair workers without SSH.
// Its secret, held only by coordinators (fleet_store.hpp), is
//   fleet_id     "svpf-" + 24 hex (random, public, in Bonjour TXT)
//   secret       256 random bits: the root of every symmetric key below
//   signing_key  a P-256 private key (fleet_crypto.hpp): only its holder can
//                sign a join, which is what lets a worker tell a coordinator
//                from another worker
//
// Derived keys (BLAKE3 derive_key; each context string is fixed and
// versioned; material is the 32-byte secret followed by canonical JSON, so
// no two inputs collide):
//   token join key   context kJoinTokenKeyContext,
//                    {"expires_at","token_id"}: the TLS-PSK of a worker's join
//                    listener until its first pairing. Every coordinator
//                    recomputes it from the token id and expiry the worker
//                    advertises; a worker that lies about either cannot
//                    complete TLS.
//   member key       context kMemberKeyContext, {"worker_join_id"}: the join
//                    listener's PSK from the first pairing on; unique to that
//                    worker, so it outlives the token and no other worker can
//                    derive it.
//   pairing id       context kPairingIdContext over
//                    {"coordinator_id","worker_join_id"} (no secret):
//                    "svpw-" + 24 hex, the same for every re-join of the same
//                    coordinator and worker, so a re-join replaces the
//                    pairing instead of adding one.
//
// Tokens are single lines, safe to paste into a shell:
//   worker token       "svpjoin1." + base64url(canonical JSON
//                      {"expires_at":<UTC seconds>,"fleet_id","fleet_public_key":
//                       "<130 hex>","join_key":"<64 hex>","token_id":"<24 hex>"})
//                      For `svp-builder worker install --join`. Carries no
//                      coordinator authority: with it a Mac can be paired, but
//                      can neither pair another Mac nor pose as a coordinator.
//                      Lifetime: kDefaultWorkerTokenLifetime unless chosen at
//                      issue (at most kMaxWorkerTokenLifetime). Coordinators
//                      refuse a first pairing after expires_at; a worker
//                      paired before then stays pairable by every coordinator
//                      (member key).
//   coordinator token  "svpfleet1." + base64url(canonical JSON
//                      {"fleet_id","fleet_secret":"<64 hex>","signing_key":
//                       "<194 hex>"})
//                      For `svp-builder workers fleet join`: the whole fleet
//                      secret. It does not expire; it is valid until the fleet
//                      is re-created. Treat it like the secret itself.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker {

inline constexpr std::string_view kFleetIdPrefix = "svpf-";
inline constexpr std::string_view kCoordinatorIdPrefix = "svpc-";
inline constexpr std::string_view kWorkerJoinIdPrefix = "svpj-";
inline constexpr std::string_view kFleetPairingIdPrefix = "svpw-";
// 96 random bits, as for pairing ids (pairing_store.hpp): opaque and far
// inside kMaxPairingIdBytes with any prefix above.
inline constexpr std::size_t kFleetIdentifierRandomBytes = 12;
// 256-bit secrets and keys, as pairing secrets (kMinPairingSecretBytes).
inline constexpr std::size_t kFleetKeyBytes = 32;

inline constexpr std::string_view kJoinTokenKeyContext = "svp fleet join token key v1";
inline constexpr std::string_view kMemberKeyContext = "svp fleet member key v1";
inline constexpr std::string_view kPairingIdContext = "svp fleet pairing id v1";

inline constexpr std::string_view kWorkerTokenPrefix = "svpjoin1.";
inline constexpr std::string_view kCoordinatorTokenPrefix = "svpfleet1.";

// A week: long enough to unbox and install a batch of new Macs over a working
// week with one token; short enough that a token copied somewhere it should
// not be stops admitting Macs soon. `workers fleet token --valid-days` picks
// another lifetime up to kMaxWorkerTokenLifetime.
inline constexpr std::chrono::hours kDefaultWorkerTokenLifetime{24 * 7};
// A quarter: beyond that a token is a standing invitation; issue a new one.
inline constexpr std::chrono::hours kMaxWorkerTokenLifetime{24 * 90};

struct FleetSecret {
  std::string fleet_id;
  std::vector<std::byte> secret;
  std::vector<std::byte> signing_key;

  bool operator==(const FleetSecret&) const = default;
};

struct WorkerJoinToken {
  std::string fleet_id;
  std::vector<std::byte> fleet_public_key;
  std::string token_id;
  // UTC seconds since the epoch.
  std::uint64_t expires_at = 0;
  std::vector<std::byte> join_key;

  bool operator==(const WorkerJoinToken&) const = default;
};

// A worker's join id: "svpj-" + the first 24 hex digits of BLAKE3 of its
// long-term P-256 public key (fleet_store.hpp WorkerJoinCredential), so only
// the holder of that key can answer for the id (fleet_join.hpp).
[[nodiscard]] std::string worker_join_id_for(const std::vector<std::byte>& worker_public_key);

// prefix + 24 random hex digits. Throws WorkerError(io) without entropy.
[[nodiscard]] std::string random_fleet_identifier(std::string_view prefix);

[[nodiscard]] FleetSecret generate_fleet_secret();
// Throws WorkerError(configuration) naming the problem.
void validate_fleet_secret(const FleetSecret& fleet);

[[nodiscard]] std::vector<std::byte> token_join_key(const FleetSecret& fleet,
                                                    std::string_view token_id,
                                                    std::uint64_t expires_at);
[[nodiscard]] std::vector<std::byte> member_key(const FleetSecret& fleet,
                                                std::string_view worker_join_id);
[[nodiscard]] std::string fleet_pairing_id(std::string_view coordinator_id,
                                           std::string_view worker_join_id);

// A new worker token valid from `now` (UTC seconds) for `lifetime`. Throws
// WorkerError(configuration) for a lifetime that is not positive or exceeds
// kMaxWorkerTokenLifetime.
[[nodiscard]] WorkerJoinToken issue_worker_token(const FleetSecret& fleet, std::uint64_t now,
                                                 std::chrono::seconds lifetime);

[[nodiscard]] std::string encode_worker_token(const WorkerJoinToken& token);
// Throws WorkerError(configuration) for anything but a well-formed worker
// token (a coordinator token is named as such).
[[nodiscard]] WorkerJoinToken decode_worker_token(std::string_view text);
[[nodiscard]] std::string encode_coordinator_token(const FleetSecret& fleet);
[[nodiscard]] FleetSecret decode_coordinator_token(std::string_view text);

// Whether `id` is prefix + 24 lowercase hex digits.
[[nodiscard]] bool is_fleet_identifier(std::string_view id, std::string_view prefix) noexcept;

// UTC seconds since the epoch now.
[[nodiscard]] std::uint64_t utc_seconds_now();

}  // namespace svp::exec::worker
