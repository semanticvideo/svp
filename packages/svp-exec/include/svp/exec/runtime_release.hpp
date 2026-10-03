#pragma once

// Release order of runtimes: which of two runtimes is "newer", so a worker's
// long-running service can move to a newer runtime by itself and never back
// (worker/service_updater.hpp).
//
// A runtime's release stamp is a file of the runtime, listed in its manifest
// like every other file (component kRuntimeReleaseComponent):
//
//   libexec/svp/runtime/release.json
//   {"release_stamp":<UTC seconds since the Unix epoch>,
//    "schema":"svp.runtime.release/1"}                 (canonical JSON)
//
// `cmake --install` writes it (svp-runtime-manifest write) with the time of
// the install, before the manifest. Because the manifest records its BLAKE3,
// the stamp is part of runtime_id and cannot change without changing the
// runtime's identity. It is a file rather than a manifest member so that
// workers whose agent predates it still accept and run such runtimes (their
// manifest decoder rejects unknown members); the manifest schema stays
// svp.runtime.manifest/1.
//
// Ordering rule, a total order over stamped runtimes that every Mac computes
// the same way from the runtimes alone:
//   1. the larger release_stamp is newer;
//   2. equal stamps: the larger runtime_id is newer, comparing its 32 digest
//      bytes as unsigned bytes, most significant first (an arbitrary but
//      shared tie-break: two installs in the same second on different Macs
//      still agree on one winner everywhere).
// An unstamped runtime (no release file: installs before the stamp existed,
// and builder_only runtimes assembled at run time) is older than every
// stamped one and is never newer than anything, so it is never an update
// target.
//
// The stamp trusts the installing Mac's clock: Macs keep UTC with network
// time, and an install on a Mac whose clock runs ahead orders its runtime
// later than real time would.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/runtime_manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svp::exec {

inline constexpr std::string_view kRuntimeReleaseSchema = "svp.runtime.release/1";
// Inside the runtime bundle directory (libexec/svp/runtime), next to
// manifest.json and components.json.
inline constexpr std::string_view kRuntimeReleaseFileName = "release.json";
inline constexpr std::string_view kRuntimeReleasePath = "libexec/svp/runtime/release.json";
inline constexpr std::string_view kRuntimeReleaseComponent = "svp-release";

struct RuntimeRelease {
  Blake3Digest runtime_id{};
  std::optional<std::uint64_t> release_stamp;

  friend bool operator==(const RuntimeRelease&, const RuntimeRelease&) = default;
};

[[nodiscard]] std::string encode_runtime_release_record(std::uint64_t release_stamp);
// Throws ExecError for anything but a canonical release record.
[[nodiscard]] std::uint64_t decode_runtime_release_record(std::string_view bytes);

// The release of the runtime at `runtime_dir` whose manifest is `manifest`:
// its runtime_id, and the stamp of its release file when the manifest lists
// one and the file on disk matches the manifest's size and BLAKE3 (otherwise
// the stamp is absent, so a damaged file never orders a runtime). Never
// throws for a missing or damaged release file.
[[nodiscard]] RuntimeRelease runtime_release_of(const RuntimeManifest& manifest,
                                                const std::filesystem::path& runtime_dir);

// True when `candidate` is newer than `current` under the rule above; false
// for an unstamped candidate and for the same runtime.
[[nodiscard]] bool is_newer_release(const RuntimeRelease& candidate,
                                    const RuntimeRelease& current) noexcept;

// The stamp an install made now would get: UTC seconds since the epoch.
[[nodiscard]] std::uint64_t release_stamp_now();

}  // namespace svp::exec
