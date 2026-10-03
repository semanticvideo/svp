#pragma once

// Pairing material on disk (plan §3.3): a random 256-bit secret and an
// opaque pairing id, written with mode 0600 into a 0700 directory on both
// sides of a pairing.
//
//   coordinator  ~/Library/Application Support/SVP/Pairings/<pairing_id>.json
//                (SVP_PAIRINGS_DIR overrides the directory)
//   worker       <worker root>/pairings/<pairing_id>.json (worker_layout.hpp)
//
// Records are canonical JSON:
//   worker      {"created_at","pairing_id","role":"worker",
//                "schema":"svp.worker.pairing/1","secret":"<64 hex>"}
//   coordinator {"created_at","pairing_id","role":"coordinator",
//                "runtime":{"kind","runtime_id"},
//                "schema":"svp.worker.pairing/1","secret":"<64 hex>",
//                "worker":{WorkerEndpoint}}
//
// Like ssh with its keys, a reader refuses a record or directory that other
// users can read or write, or that another user owns.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/host_facts.hpp"
#include "svp/exec/worker/worker_layout.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker {

inline constexpr std::string_view kPairingRecordSchema = "svp.worker.pairing/1";
inline constexpr std::string_view kPairingsDirEnvironmentVariable = "SVP_PAIRINGS_DIR";
// Pairing ids are "svpw-" and 24 hex digits (96 random bits): opaque, never
// derived from a host name or address, and far inside kMaxPairingIdBytes.
inline constexpr std::string_view kPairingIdPrefix = "svpw-";
inline constexpr std::size_t kPairingIdRandomBytes = 12;

// A fresh pairing: random id and a kMinPairingSecretBytes (256-bit) secret
// from the system CSPRNG. Throws WorkerError(io) when no entropy is
// available.
[[nodiscard]] svp::exec::remote::PairingKey generate_pairing_key();

[[nodiscard]] std::string secret_hex(const std::vector<std::byte>& secret);

struct WorkerPairingRecord {
  svp::exec::remote::PairingKey key;
  // UTC, "YYYY-MM-DDTHH:MM:SSZ".
  std::string created_at;
};

// Everything the coordinator learned about the worker at pairing time and
// needs again to reach, report, or unpair it.
struct WorkerEndpoint {
  // What the user passed to `workers pair` (user@host); used only for SSH
  // during pairing and unpairing, never to find the worker for work
  // (discovery is by pairing id, plan §3.4).
  std::string ssh_target;
  std::string user;
  std::uint32_t uid = 0;
  std::string home;
  std::string root;
  WorkerServiceMode service_mode = WorkerServiceMode::user_agent;
  std::string label;
  std::string plist;
  std::string arch;
  OsIdentity os;
  // The worker's fleet join id when it was paired through its fleet join
  // listener (fleet_join.hpp) instead of SSH; ssh_target is then empty.
  // Stored only when set ("join_id" in the record's "worker" object).
  std::string join_id;

  bool operator==(const WorkerEndpoint&) const = default;
};

struct CoordinatorPairingRecord {
  svp::exec::remote::PairingKey key;
  std::string created_at;
  WorkerEndpoint worker;
  // The runtime installed at pairing.
  Blake3Digest runtime_id{};
  RuntimeKind runtime_kind = RuntimeKind::bundle;
};

[[nodiscard]] std::string encode_worker_pairing(const WorkerPairingRecord& record);
[[nodiscard]] WorkerPairingRecord decode_worker_pairing(std::string_view bytes);
[[nodiscard]] std::string encode_coordinator_pairing(const CoordinatorPairingRecord& record);
[[nodiscard]] CoordinatorPairingRecord decode_coordinator_pairing(std::string_view bytes);

[[nodiscard]] std::string utc_timestamp_now();

// A 0700 directory of 0600 "<pairing_id>.json" files owned by this user.
class PairingDirectory {
 public:
  explicit PairingDirectory(std::filesystem::path directory);

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return directory_; }
  [[nodiscard]] std::filesystem::path file_for(std::string_view pairing_id) const;

  // Creates the directory (0700) when needed and replaces the record
  // atomically (temp file 0600, fsync, rename). Throws WorkerError(io,
  // configuration).
  void write(std::string_view pairing_id, std::string_view bytes) const;
  // nullopt when absent. Throws WorkerError(configuration) for a record with
  // unsafe ownership or permissions.
  [[nodiscard]] std::optional<std::string> read(std::string_view pairing_id) const;
  // Every record, in pairing id order. A missing directory has none.
  [[nodiscard]] std::vector<std::string> read_all() const;
  // True when a record was removed.
  bool remove(std::string_view pairing_id) const;

 private:
  void check_directory() const;
  std::filesystem::path directory_;
};

// $SVP_PAIRINGS_DIR, else ~/Library/Application Support/SVP/Pairings.
[[nodiscard]] std::filesystem::path default_coordinator_pairings_dir();

[[nodiscard]] std::vector<CoordinatorPairingRecord> load_coordinator_pairings(
    const PairingDirectory& directory);
[[nodiscard]] std::vector<WorkerPairingRecord> load_worker_pairings(
    const PairingDirectory& directory);

}  // namespace svp::exec::worker
