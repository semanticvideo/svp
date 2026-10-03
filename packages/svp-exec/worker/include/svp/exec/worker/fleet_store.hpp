#pragma once

// Fleet material on disk, 0600 files in 0700 directories owned by the user
// (PairingDirectory's rules, pairing_store.hpp):
//
//   coordinator  ~/Library/Application Support/SVP/Fleet/fleet.json
//                (SVP_FLEET_DIR overrides the directory)
//                {"coordinator_id":"svpc-<24 hex>","created_at","fleet_id",
//                 "fleet_secret":"<64 hex>","schema":"svp.fleet/1",
//                 "signing_key":"<194 hex>"}
//                coordinator_id is this Mac's own, never shared: two
//                coordinators of one fleet pair a worker separately.
//   worker       <worker root>/join.json (worker_layout.hpp)
//                {"created_at","fleet_id","fleet_public_key":"<130 hex>",
//                 "member_key":"<64 hex>",        after the first pairing
//                 "schema":"svp.worker.join/1",
//                 "token":{"expires_at","join_key","token_id"},
//                 "worker_join_id":"svpj-<24 hex>"}
//
// Neither is ever passed on a command line.

#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/worker/fleet_keys.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker {

inline constexpr std::string_view kFleetRecordSchema = "svp.fleet/1";
inline constexpr std::string_view kWorkerJoinSchema = "svp.worker.join/1";
inline constexpr std::string_view kFleetDirEnvironmentVariable = "SVP_FLEET_DIR";

struct FleetMembership {
  FleetSecret fleet;
  std::string coordinator_id;
  std::string created_at;

  bool operator==(const FleetMembership&) const = default;
};

[[nodiscard]] std::string encode_fleet_membership(const FleetMembership& membership);
// Throws WorkerError(configuration).
[[nodiscard]] FleetMembership decode_fleet_membership(std::string_view bytes);

// $SVP_FLEET_DIR, else ~/Library/Application Support/SVP/Fleet.
[[nodiscard]] std::filesystem::path default_fleet_dir();
[[nodiscard]] std::optional<FleetMembership> load_fleet_membership(
    const std::filesystem::path& directory);
void save_fleet_membership(const std::filesystem::path& directory,
                           const FleetMembership& membership);

struct WorkerJoinCredential {
  std::string worker_join_id;
  WorkerJoinToken token;
  // Set by the first pairing; from then on the join listener's key.
  std::optional<std::vector<std::byte>> member_key;
  std::string created_at;

  bool operator==(const WorkerJoinCredential&) const = default;
};

[[nodiscard]] std::string encode_worker_join_credential(const WorkerJoinCredential& credential);
[[nodiscard]] WorkerJoinCredential decode_worker_join_credential(std::string_view bytes);

[[nodiscard]] std::optional<WorkerJoinCredential> load_worker_join_credential(
    const WorkerLayout& layout);
void save_worker_join_credential(const WorkerLayout& layout,
                                 const WorkerJoinCredential& credential);

// The TLS-PSK of the worker's join listener: identity worker_join_id, key
// the member key once set, else the token's join key.
[[nodiscard]] svp::exec::remote::PairingKey join_listener_key(
    const WorkerJoinCredential& credential);

}  // namespace svp::exec::worker
