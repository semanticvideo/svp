#pragma once

// Runs one external command (ffmpeg, ffprobe) for the working copy, without a
// shell, and stops it when the build is cancelled. Making a working copy of a
// long video takes minutes, so unlike the builder's short ffmpeg calls it
// must not outlive a Ctrl-C.

#include "svp/exec/cancellation_token.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace svp::builder::working_copy {

// How often a running command checks for cancellation: far inside the
// builder's interrupt grace period (build_interrupt.hpp), so a cancelled
// build stops its command before the process is ended for it.
inline constexpr std::chrono::milliseconds kCommandCancellationPollInterval{100};

struct CommandOutcome {
  // The program could not be started (not found, not executable).
  bool started = false;
  // The build was cancelled and the command was stopped.
  bool cancelled = false;
  // The command exited by itself with status 0.
  bool succeeded = false;
  // Why it did not succeed: the program's standard error, or how it ended.
  std::string diagnostics;
};

// Runs argv[0] (searched on PATH when it has no '/') with standard input from
// /dev/null, standard output written to `stdout_path`, and standard error to
// `stderr_path`. The child starts with SIGINT and SIGTERM unblocked and at
// their default actions (the builder blocks them in every thread). Once
// `cancellation` is requested the child is sent SIGTERM and waited for.
[[nodiscard]] CommandOutcome run_cancellable_command(
    const std::vector<std::string>& argv, const std::filesystem::path& stdout_path,
    const std::filesystem::path& stderr_path,
    const svp::exec::CancellationToken* cancellation);

}  // namespace svp::builder::working_copy
