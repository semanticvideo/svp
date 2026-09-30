#pragma once

// Rows the recovery journal records. The first group mirrors the RC2 §20.4
// minimum tables; the second is the non-normative distributed-execution
// extension from plan §4.6 (worker_session, task_attempt, blob_location).

#include "svp/exec/blake3_digest.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace svp::exec {

enum class BuildSessionStatus { active, succeeded, failed, interrupted };

[[nodiscard]] std::string_view build_session_status_name(BuildSessionStatus status) noexcept;

// `build_session` row.
struct BuildSessionRecord {
  std::string id;
  std::string started_utc;
  std::string svp_version;
  std::string builder_version;
  BuildSessionStatus status = BuildSessionStatus::active;
};

// `source_fingerprint` row. resume() compares source_id, size_bytes, and
// blake3; path and mtime_ns are informational (the file may have moved).
struct SourceFingerprintRecord {
  std::string source_id;
  std::string path;
  std::uint64_t size_bytes = 0;
  std::optional<std::int64_t> mtime_ns;
  Blake3Digest blake3{};

  bool operator==(const SourceFingerprintRecord&) const = default;
};

// `task` row (inserted as `planned`) plus its `task_dependency` rows.
struct JournalTaskRecord {
  std::string task_id;
  std::string task_type;
  std::optional<Blake3Digest> cache_key;
  std::vector<std::string> depends_on;
};

// `artifact_provenance` row.
struct ArtifactProvenanceRecord {
  std::string processor_id;
  Blake3Digest parameters_blake3{};
  std::vector<std::string> model_refs;
};

// One task output to commit. `blake3` is the digest the producer declared;
// the journal re-verifies the bytes it wrote against it.
struct JournalArtifactInput {
  std::string artifact_id;
  Blake3Digest blake3{};
  // In-memory bytes (borrowed for the call) or a file to copy.
  std::variant<std::span<const std::byte>, std::filesystem::path> content;
  ArtifactProvenanceRecord provenance;
};

// `cache_hit` row. `artifact_id` must be one of the commit's artifacts.
struct CacheHitRecord {
  Blake3Digest cache_key{};
  std::string artifact_id;
  bool verified = false;
};

struct TaskCommit {
  std::string task_id;
  Blake3Digest output_blake3{};
  std::vector<JournalArtifactInput> artifacts;
  std::optional<CacheHitRecord> cache_hit;
};

// One committed `artifact` row, read back. `path` is the absolute path of its
// blob under blobs/completed/; callers re-verify the bytes they read.
struct JournalArtifactRecord {
  std::string artifact_id;
  Blake3Digest blake3{};
  std::uint64_t byte_length = 0;
  std::filesystem::path path;
};

// A task the journal holds as `committed`, with its output digest and
// artifacts (sorted by artifact_id).
struct JournalCommittedTask {
  std::string task_id;
  Blake3Digest output_blake3{};
  std::vector<JournalArtifactRecord> artifacts;
};

// --- Distributed execution (non-normative, plan §4.6) ---

enum class WorkerSessionStatus { active, ended, lost, quarantined };

[[nodiscard]] std::string_view worker_session_status_name(WorkerSessionStatus status) noexcept;

// `worker_session` row. A local build records one session for itself.
struct WorkerSessionRecord {
  std::string worker_session_id;
  std::string worker_id;
  std::optional<Blake3Digest> runtime_id;
  std::string started_utc;
  std::optional<std::string> ended_utc;
  WorkerSessionStatus status = WorkerSessionStatus::active;
};

enum class AttemptOutcome { leased, running, succeeded, failed, lost, abandoned, duplicate };

[[nodiscard]] std::string_view attempt_outcome_name(AttemptOutcome outcome) noexcept;

// `task_attempt` row, keyed by (task_id, attempt); re-recording replaces it.
struct TaskAttemptRecord {
  std::string task_id;
  std::uint32_t attempt = 0;
  std::optional<std::string> worker_session_id;
  std::optional<std::string> lease_id;
  std::optional<std::string> leased_utc;
  std::optional<std::string> started_utc;
  std::optional<std::string> completed_utc;
  std::optional<std::uint64_t> queue_ms;
  std::optional<std::uint64_t> transfer_ms;
  std::optional<std::uint64_t> compute_ms;
  std::optional<std::uint64_t> cpu_ms;
  std::optional<std::uint64_t> peak_rss_bytes;
  AttemptOutcome outcome = AttemptOutcome::leased;
  std::optional<std::string> error;
};

// Location recorded for the coordinator's own journal blobs.
inline constexpr std::string_view kJournalBlobLocation = "journal";

// `blob_location` row: which store holds a blob. `location` is
// kJournalBlobLocation or a worker_session_id.
struct BlobLocationRecord {
  Blake3Digest blake3{};
  std::string location;
  std::uint64_t byte_length = 0;
};

}  // namespace svp::exec
