#pragma once

// The system ssh, run in the user's terminal (plan §3.3 step 1). SVP never
// sees a password: ssh and sudo prompt on the terminal themselves, and keys
// or an agent the user already has work unchanged. One control connection
// (ControlMaster) is shared by every command of a pairing so the user signs
// in once.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::workers {

inline constexpr std::string_view kSystemSsh = "/usr/bin/ssh";
inline constexpr std::string_view kSystemTar = "/usr/bin/tar";

// How long the shared control connection outlives its last command: long
// enough to span the gaps between pairing steps (a model push can take
// minutes, so steps reconnect when it has expired), short enough that no
// connection lingers after svp-builder exits.
inline constexpr int kSshControlPersistSeconds = 60;

struct SshTarget {
  std::string destination;  // user@host
  // Extra `-o` options passed through to ssh (for example an IdentityFile).
  std::vector<std::string> options;
};

class SshSession {
 public:
  explicit SshSession(SshTarget target);
  ~SshSession();
  SshSession(const SshSession&) = delete;
  SshSession& operator=(const SshSession&) = delete;

  [[nodiscard]] const SshTarget& target() const noexcept { return target_; }

  // Runs `script` with /bin/sh on the worker, `input` on its stdin; returns
  // its stdout. stderr goes to this terminal. Throws WorkerError(command)
  // when ssh or the script fails.
  std::string run_script(const std::string& script, std::string_view input = {});

  // Same, with stdin streamed from a local program's stdout (a tar stream).
  std::string run_script_fed_by(const std::string& script,
                                const std::vector<std::string>& local_program);

  // Runs `remote_command` with a terminal allocated and this process's
  // stdin, so sudo can ask for the administrator password. Throws
  // WorkerError(command) on failure.
  void run_interactive(const std::string& remote_command);

 private:
  [[nodiscard]] std::vector<std::string> ssh_arguments(bool terminal) const;

  SshTarget target_;
  std::filesystem::path control_dir_;
};

}  // namespace svp::builder::workers
