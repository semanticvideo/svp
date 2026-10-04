#pragma once

// Test-starting a runtime before the worker service moves to it
// (service_updater.hpp): its own svp-builder must start on this Mac and
// verify its runtime directory,
//
//   <runtime>/bin/svp-builder worker verify-runtime --runtime-dir <runtime>
//       --expect b3:<hex>
//
// exiting 0 and printing "runtime_id=b3:<hex>" for the expected id within a
// deadline. That proves the program loads (dyld, its libraries, its
// architecture and macOS target) and reads its files, which a
// verification in the running service alone cannot.

#include "svp/exec/blake3_digest.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace svp::exec::worker {

// Deadline: kTestStartLaunchAllowance plus the runtime's bytes at
// kTestStartMinVerifyBytesPerSecond, so it scales with the runtime instead
// of assuming one size.
//   kTestStartLaunchAllowance: process start, dyld binding, and argument
//     parsing take well under a second on any Apple Silicon Mac; 10 s also
//     covers a Mac busy with other work (the test-start runs only while no
//     session is live, but the Mac's own build may be running).
//   kTestStartMinVerifyBytesPerSecond: verification reads and BLAKE3-hashes
//     every file; 50 MB/s is far below what any Apple Silicon SSD and BLAKE3
//     sustain, so only a hung or broken program reaches the deadline.
inline constexpr std::chrono::seconds kTestStartLaunchAllowance{10};
inline constexpr std::uint64_t kTestStartMinVerifyBytesPerSecond = 50ULL * 1000 * 1000;

[[nodiscard]] std::chrono::milliseconds test_start_deadline(std::uint64_t runtime_bytes);

struct TestStartOutcome {
  bool passed = false;
  // Why it failed, for the service log; empty when it passed.
  std::string reason;
};

// Runs the runtime's own svp-builder as above. Never throws.
[[nodiscard]] TestStartOutcome test_start_runtime(const std::filesystem::path& runtime_dir,
                                                  const Blake3Digest& runtime_id,
                                                  std::chrono::milliseconds deadline);

}  // namespace svp::exec::worker
