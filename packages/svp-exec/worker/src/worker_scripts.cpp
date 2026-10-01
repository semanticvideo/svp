#include "svp/exec/worker/worker_scripts.hpp"

#include "svp/exec/worker/worker_error.hpp"

#include <charconv>
#include <cstdio>
#include <sstream>

namespace svp::exec::worker {
namespace {

std::string q(const std::filesystem::path& path) { return shell_quote(path.string()); }

// launchd may still be tearing a booted-out job down; bootstrap then fails
// with an I/O error for a moment. Retried once a second, up to this many
// times (launchd's own exit timeout for a job is 20 s).
constexpr int kBootstrapAttempts = 20;

std::string bootstrap_with_retry(std::string_view domain, std::string_view plist_variable) {
  std::ostringstream out;
  out << "attempt=0\n"
      << "until launchctl bootstrap " << domain << " \"" << plist_variable << "\"; do\n"
      << "  attempt=$((attempt + 1))\n"
      << "  if [ \"$attempt\" -ge " << kBootstrapAttempts << " ]; then\n"
      << "    echo \"svp: launchctl bootstrap failed\" >&2; exit 1\n"
      << "  fi\n"
      << "  sleep 1\n"
      << "done\n";
  return out.str();
}

std::uint64_t parse_unsigned(const std::string& value, std::string_view key) {
  std::uint64_t number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc{} || end != value.data() + value.size() || value.empty()) {
    throw WorkerError(WorkerErrorCode::command,
                      "worker probe reported a malformed " + std::string(key) + ": `" + value +
                          "`");
  }
  return number;
}

}  // namespace

std::string shell_quote(std::string_view text) {
  std::string quoted = "'";
  for (const char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

std::string render_probe_script(std::string_view label) {
  const std::string agent_root =
      "$HOME/Library/Application Support/SVP/Worker";
  const std::string daemon_root =
      default_worker_root(WorkerServiceMode::system_daemon, "/").string();
  const std::string daemon_plist =
      launchd_plist_path(WorkerServiceMode::system_daemon, "/", label).string();
  std::ostringstream out;
  out << "set -u\n"
      << "avail() { df -Pk \"$1\" 2>/dev/null | awk 'NR==2 {printf \"%.0f\\n\", $4 * 1024}'; }\n"
      << "count() { ls \"$1\" 2>/dev/null | grep -c '\\.json$' || true; }\n"
      << "echo \"arch=$(uname -m)\"\n"
      << "echo \"product_version=$(sw_vers -productVersion)\"\n"
      << "echo \"build=$(sw_vers -buildVersion)\"\n"
      << "echo \"user=$(id -un)\"\n"
      << "echo \"uid=$(id -u)\"\n"
      << "echo \"home=$HOME\"\n"
      << "echo \"home_available_bytes=$(avail \"$HOME\")\"\n"
      << "echo \"library_available_bytes=$(avail /Library)\"\n"
      << "echo \"user_agent_pairings=$(count \"" << agent_root << "/pairings\")\"\n"
      << "echo \"system_daemon_pairings=$(count " << shell_quote(daemon_root + "/pairings")
      << ")\"\n"
      << "if launchctl print \"gui/$(id -u)/" << label
      << "\" >/dev/null 2>&1; then echo user_agent_job=loaded; else echo user_agent_job=absent; fi\n"
      << "if [ -f " << shell_quote(daemon_plist)
      << " ]; then echo system_daemon_plist=present; else echo system_daemon_plist=absent; fi\n";
  return out.str();
}

WorkerProbe parse_probe_output(std::string_view output) {
  std::map<std::string, std::string> values;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    const auto equals = line.find('=');
    if (equals != std::string::npos) {
      values[line.substr(0, equals)] = line.substr(equals + 1);
    }
  }
  const auto get = [&](std::string_view key) -> const std::string& {
    const auto found = values.find(std::string(key));
    if (found == values.end() || found->second.empty()) {
      throw WorkerError(WorkerErrorCode::command,
                        "worker probe did not report " + std::string(key));
    }
    return found->second;
  };
  WorkerProbe probe;
  probe.arch = get("arch");
  probe.product_version = get("product_version");
  probe.build = get("build");
  probe.user = get("user");
  probe.uid = static_cast<std::uint32_t>(parse_unsigned(get("uid"), "uid"));
  probe.home = get("home");
  probe.home_available_bytes = parse_unsigned(get("home_available_bytes"), "home_available_bytes");
  probe.library_available_bytes =
      parse_unsigned(get("library_available_bytes"), "library_available_bytes");
  probe.user_agent_pairings = parse_unsigned(get("user_agent_pairings"), "user_agent_pairings");
  probe.system_daemon_pairings =
      parse_unsigned(get("system_daemon_pairings"), "system_daemon_pairings");
  probe.user_agent_job_loaded = get("user_agent_job") == "loaded";
  probe.system_daemon_plist_present = get("system_daemon_plist") == "present";
  return probe;
}

std::string render_receive_runtime_script(const std::filesystem::path& runtimes_dir,
                                          const Blake3Digest& runtime_id) {
  const std::string hex = blake3_hex(runtime_id);
  const std::string expected = blake3_prefixed(runtime_id);
  const std::string program = std::string(kSessionProgram);
  std::ostringstream out;
  out << "set -eu\n"
      << "umask 077\n"
      << "dest=" << q(runtimes_dir) << "\n"
      << "target=\"$dest/" << hex << "\"\n"
      << "stage=\"$dest/.incoming-" << hex << "-$$\"\n"
      << "mkdir -p \"$dest\"\n"
      << "rm -rf \"$stage\"\n"
      << "mkdir \"$stage\"\n"
      << "trap 'rm -rf \"$stage\"' EXIT\n"
      << "tar -xpf - -C \"$stage\"\n"
      << "\"$stage/" << program << "\" worker verify-runtime --runtime-dir \"$stage\" --expect "
      << expected << " >/dev/null\n"
      << "if [ -d \"$target\" ] && \"$target/" << program
      << "\" worker verify-runtime --runtime-dir \"$target\" --expect " << expected
      << " >/dev/null 2>&1; then\n"
      << "  :\n"
      << "else\n"
      << "  rm -rf \"$target\"\n"
      << "  mv \"$stage\" \"$target\"\n"
      << "fi\n"
      << "echo \"runtime_id=" << expected << "\"\n";
  return out.str();
}

std::string render_write_file_script(const std::filesystem::path& path, unsigned mode) {
  char octal[8];
  std::snprintf(octal, sizeof(octal), "%o", mode);
  std::ostringstream out;
  out << "set -eu\n"
      << "umask 077\n"
      << "target=" << q(path) << "\n"
      << "mkdir -p \"$(dirname \"$target\")\"\n"
      << "tmp=\"$target.tmp-$$\"\n"
      << "cat > \"$tmp\"\n"
      << "chmod " << octal << " \"$tmp\"\n"
      << "mv -f \"$tmp\" \"$target\"\n";
  return out.str();
}

std::string render_prepare_root_script(const std::filesystem::path& root) {
  const WorkerLayout layout{.root = root};
  std::ostringstream out;
  out << "set -eu\n"
      << "umask 077\n"
      << "mkdir -p " << q(layout.root) << " " << q(layout.pairings()) << " "
      << q(layout.runtimes()) << " " << q(layout.models()) << " " << q(layout.cache()) << " "
      << q(layout.logs()) << "\n"
      << "chmod 700 " << q(layout.root) << "\n";
  return out.str();
}

std::string render_agent_start_script(const std::filesystem::path& plist,
                                      const std::filesystem::path& root, std::string_view label) {
  std::ostringstream out;
  out << "set -eu\n"
      << "uid=$(id -u)\n"
      << "plist=" << q(plist) << "\n"
      << "agents=\"$(dirname \"$plist\")\"\n"
      << "if [ ! -d \"$agents\" ]; then\n"
      << "  mkdir -p \"$agents\"\n"
      << "  : > " << q(root / std::string(kCreatedLaunchAgentsMarker)) << "\n"
      << "fi\n"
      << "launchctl bootout \"gui/$uid/" << label << "\" >/dev/null 2>&1 || true\n"
      << bootstrap_with_retry("\"gui/$uid\"", "$plist")
      << "launchctl print \"gui/$uid/" << label << "\" >/dev/null\n";
  return out.str();
}

std::string render_daemon_install_script(const DaemonInstall& install) {
  const std::string hex = blake3_hex(install.runtime_id);
  const WorkerLayout layout{.root = install.root};
  std::ostringstream out;
  out << "#!/bin/sh\n"
      << "# Installs the SVP worker LaunchDaemon. Run with sudo.\n"
      << "set -eu\n"
      << "umask 022\n"
      << "user=" << shell_quote(install.user) << "\n"
      << "group=$(id -gn \"$user\")\n"
      << "staging=" << q(install.staging) << "\n"
      << "root=" << q(install.root) << "\n"
      << "plist=" << q(install.plist) << "\n"
      << "label=" << shell_quote(install.label) << "\n"
      << "if [ \"$(id -u)\" != 0 ]; then echo 'svp: run with sudo' >&2; exit 1; fi\n"
      << "mkdir -p \"$(dirname \"$root\")\"\n"
      << "for dir in \"$root\" " << q(layout.pairings()) << " " << q(layout.runtimes()) << " "
      << q(layout.models()) << " " << q(layout.cache()) << " " << q(layout.logs()) << "; do\n"
      << "  install -d -m 0700 -o \"$user\" -g \"$group\" \"$dir\"\n"
      << "done\n"
      << "if [ ! -d \"$root/runtimes/" << hex << "\" ]; then\n"
      << "  mv \"$staging/runtimes/" << hex << "\" \"$root/runtimes/" << hex << "\"\n"
      << "fi\n"
      << "for record in \"$staging\"/pairings/*.json; do\n"
      << "  [ -f \"$record\" ] && mv -f \"$record\" \"$root/pairings/\"\n"
      << "done\n"
      << "chown -R \"$user:$group\" \"$root\"\n"
      << "install -m 0644 -o root -g wheel \"$staging/$label.plist\" \"$plist\"\n"
      << "launchctl bootout \"system/$label\" >/dev/null 2>&1 || true\n"
      << bootstrap_with_retry("system", "$plist")
      << "launchctl print \"system/$label\" >/dev/null\n"
      << "rm -rf \"$staging\"\n"
      << "rmdir \"$(dirname \"$staging\")\" 2>/dev/null || true\n";
  return out.str();
}

std::string render_removal_script(const WorkerRemoval& removal) {
  const bool daemon = removal.mode == WorkerServiceMode::system_daemon;
  std::ostringstream out;
  out << "#!/bin/sh\n"
      << "set -u\n"
      << "root=" << q(removal.root) << "\n"
      << "plist=" << q(removal.plist) << "\n"
      << "label=" << shell_quote(removal.label) << "\n"
      << "pairing=" << shell_quote(removal.pairing_id) << "\n";
  if (daemon) {
    out << "if [ \"$(id -u)\" != 0 ]; then echo 'svp: run with sudo' >&2; exit 1; fi\n"
        << "service=\"system/$label\"\n";
  } else {
    out << "service=\"gui/$(id -u)/$label\"\n";
  }
  out << "rm -f \"$root/pairings/$pairing.json\"\n"
      << "remaining=$(ls \"$root/pairings\" 2>/dev/null | grep -c '\\.json$' || true)\n"
      << "if [ \"$remaining\" = 0 ]; then\n"
      << "  launchctl bootout \"$service\" >/dev/null 2>&1 || true\n"
      << "  rm -f \"$plist\"\n";
  if (!daemon) {
    out << "  created_agents_dir=no\n"
        << "  [ -f \"$root/" << kCreatedLaunchAgentsMarker
        << "\" ] && created_agents_dir=yes\n";
  }
  out << "  rm -rf \"$root\"\n"
      << "  rmdir \"$(dirname \"$root\")\" 2>/dev/null || true\n";
  if (!daemon) {
    out << "  if [ \"$created_agents_dir\" = yes ]; then rmdir \"$(dirname \"$plist\")\" "
           "2>/dev/null || true; fi\n";
  }
  out << "  echo removed=all\n"
      << "else\n"
      << "  launchctl kickstart -k \"$service\" >/dev/null 2>&1 || true\n"
      << "  echo \"removed=pairing remaining=$remaining\"\n"
      << "fi\n";
  return out.str();
}

}  // namespace svp::exec::worker
