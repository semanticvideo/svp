// The launchd job (plan §3.3) and the shell scripts pairing runs on a
// worker: property lists must pass `plutil -lint`, scripts must parse with
// `/bin/sh -n`, and the scripts that only touch files are run for real
// against a scratch root.

#include "svp/exec/worker/launchd_job.hpp"
#include "svp/exec/worker/runtime_source.hpp"
#include "svp/exec/worker/worker_scripts.hpp"
#include "worker_test_support.hpp"

#include <sys/stat.h>

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
namespace fs = std::filesystem;

const svp::exec::Blake3Digest kRuntime = svp::exec::blake3_digest(std::string_view("runtime"));

void expect_plutil_lint(const fs::path& directory, const std::string& plist) {
  const fs::path file = directory / "job.plist";
  write_file(file, plist);
  const auto [status, output] = run_command("/usr/bin/plutil -lint " + shell_quote(file.string()));
  expect(status == 0, "plutil -lint accepts the plist: " + output);
}

void expect_sh_parses(const fs::path& directory, const std::string& script) {
  const fs::path file = directory / "script.sh";
  write_file(file, script);
  const auto [status, output] = run_command("/bin/sh -n " + shell_quote(file.string()) + " 2>&1");
  expect(status == 0, "sh -n accepts the script: " + output + "\n" + script);
}

std::string plutil_extract(const fs::path& directory, const std::string& plist,
                           const std::string& key) {
  const fs::path file = directory / "extract.plist";
  write_file(file, plist);
  const auto [status, output] =
      run_command("/usr/bin/plutil -extract " + shell_quote(key) + " raw -o - " +
                  shell_quote(file.string()) + " 2>/dev/null");
  if (status != 0) {
    return "<missing>";
  }
  std::string value = output;
  while (!value.empty() && value.back() == '\n') {
    value.pop_back();
  }
  return value;
}

void test_agent_plist_lints_and_runs_the_runtime() {
  TemporaryDirectory scratch("svp-launchd");
  const WorkerLayout layout{.root = "/Users/w/Library/Application Support/SVP/Worker"};
  const WorkerServiceSpec spec =
      make_worker_service_spec(WorkerServiceMode::user_agent, layout, "w", "/Users/w");
  const std::string plist = render_launchd_plist(spec);
  expect_plutil_lint(scratch.path, plist);
  expect_equal(plutil_extract(scratch.path, plist, "Label"), std::string(kWorkerJobLabel),
               "label");
  expect_equal(plutil_extract(scratch.path, plist, "ProgramArguments.0"),
               (layout.root / "current/bin/svp-builder").string(),
               "program is the svp-builder of the runtime <root>/current names");
  expect_equal(plutil_extract(scratch.path, plist, "ProgramArguments.4"), layout.root.string(),
               "root passed to the agent");
  expect_equal(plutil_extract(scratch.path, plist, "KeepAlive.SuccessfulExit"), "false",
               "restarted only after a crash");
  expect_equal(plutil_extract(scratch.path, plist, "Umask"), "63", "umask 077");
  expect_equal(plutil_extract(scratch.path, plist, "UserName"), "<missing>",
               "an agent runs as its own user");
  expect_equal(plutil_extract(scratch.path, plist, "EnvironmentVariables.PATH"), "<missing>",
               "without a recorded PATH the agent keeps launchd's environment");

  const WorkerServiceSpec with_path =
      make_worker_service_spec(WorkerServiceMode::user_agent, layout, "w", "/Users/w",
                               "/opt/tools/bin:/usr/bin:/bin");
  const std::string path_plist = render_launchd_plist(with_path);
  expect_plutil_lint(scratch.path, path_plist);
  expect_equal(plutil_extract(scratch.path, path_plist, "EnvironmentVariables.PATH"),
               "/opt/tools/bin:/usr/bin:/bin", "the worker user's login PATH");
  expect_equal(plutil_extract(scratch.path, path_plist, "EnvironmentVariables.HOME"), "<missing>",
               "an agent inherits HOME from its login session");
}

void test_daemon_plist_runs_as_the_worker_user() {
  TemporaryDirectory scratch("svp-launchd");
  const WorkerLayout layout{.root = default_worker_root(WorkerServiceMode::system_daemon, "/")};
  const WorkerServiceSpec spec =
      make_worker_service_spec(WorkerServiceMode::system_daemon, layout, "w & co", "/Users/w");
  const std::string plist = render_launchd_plist(spec);
  expect_plutil_lint(scratch.path, plist);
  // The layout every worker Mac's LaunchDaemon uses (migrated Macs included).
  const std::vector<std::string> expected_arguments = {
      "/Library/Application Support/SVP/Worker/current/bin/svp-builder", "worker", "serve",
      "--root", "/Library/Application Support/SVP/Worker"};
  for (std::size_t index = 0; index < expected_arguments.size(); ++index) {
    expect_equal(plutil_extract(scratch.path, plist, "ProgramArguments." + std::to_string(index)),
                 expected_arguments[index], "daemon ProgramArguments " + std::to_string(index));
  }
  expect_equal(plutil_extract(scratch.path, plist, "ProgramArguments.5"), "<missing>",
               "no further arguments");
  expect_equal(plutil_extract(scratch.path, plist, "KeepAlive.SuccessfulExit"), "false",
               "a self-update exit (nonzero) restarts the daemon");
  expect_equal(plutil_extract(scratch.path, plist, "UserName"), "w & co",
               "UserName is the worker user, XML-escaped");
  expect_equal(plutil_extract(scratch.path, plist, "EnvironmentVariables.HOME"), "/Users/w",
               "HOME is the worker user's home");
  expect_equal(launchd_plist_path(WorkerServiceMode::system_daemon, "/Users/w").string(),
               "/Library/LaunchDaemons/org.svp.worker.plist", "daemon plist location");
  expect_equal(layout.root.string(), "/Library/Application Support/SVP/Worker", "daemon root");

  WorkerServiceSpec missing_user = spec;
  missing_user.user_name.clear();
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)render_launchd_plist(missing_user); },
                      "a daemon without a user is refused");
}

void test_scripts_parse() {
  TemporaryDirectory scratch("svp-scripts");
  const WorkerLayout layout{.root = scratch.path / "it's a root"};
  expect_sh_parses(scratch.path, render_probe_script());
  expect_sh_parses(scratch.path, render_receive_runtime_script(layout.runtimes(), kRuntime));
  expect_sh_parses(scratch.path, render_write_file_script(layout.pairings() / "x.json", 0600));
  expect_sh_parses(scratch.path, render_prepare_root_script(layout.root));
  expect_sh_parses(scratch.path, render_point_current_script(layout.root, kRuntime));
  expect_sh_parses(scratch.path, render_agent_start_script(scratch.path / "a.plist"));
  expect_sh_parses(scratch.path,
                   render_prepare_launch_agents_script(scratch.path / "a.plist", layout.root));
  expect_sh_parses(scratch.path,
                   render_daemon_install_script(DaemonInstall{
                       .user = "w",
                       .staging = scratch.path / "staging",
                       .root = default_worker_root(WorkerServiceMode::system_daemon, "/"),
                       .plist = launchd_plist_path(WorkerServiceMode::system_daemon, "/"),
                       .runtime_id = kRuntime}));
  for (const WorkerServiceMode mode :
       {WorkerServiceMode::user_agent, WorkerServiceMode::system_daemon}) {
    expect_sh_parses(scratch.path, render_removal_script(WorkerRemoval{
                                       .mode = mode,
                                       .root = layout.root,
                                       .plist = scratch.path / "a.plist",
                                       .pairing_id = "svpw-0123"}));
  }
}

void test_shell_quote_survives_the_shell() {
  const std::string tricky = "it's \"quoted\" $HOME `x` \\ end";
  const auto [status, output] = run_command("/bin/sh -c " + shell_quote("printf %s " + shell_quote(tricky)));
  expect(status == 0 && output == tricky, "shell_quote round trips: " + output);
}

void test_probe_runs_here() {
  const auto [status, output] = run_command("/bin/sh -c " + shell_quote(render_probe_script()));
  expect(status == 0, "probe script exits 0");
  const WorkerProbe probe = parse_probe_output(output);
  const HostFacts host = detect_host_facts();
  expect_equal(probe.arch, host.arch, "probe arch matches this Mac");
  expect_equal(probe.product_version, host.os.product_version, "probe macOS matches");
  expect(probe.uid == ::getuid(), "probe uid matches");
  expect(probe.home_available_bytes > 0, "probe reports free disk");
  expect(probe.login_path.find("/usr/bin") != std::string::npos,
         "probe reports the login shell's PATH: " + probe.login_path);
  const WorkerProbe without_path = parse_probe_output(
      "arch=arm64\nproduct_version=1\nbuild=b\nuser=u\nuid=1\nhome=/h\n"
      "home_available_bytes=1\nlibrary_available_bytes=1\nuser_agent_pairings=0\n"
      "system_daemon_pairings=0\nuser_agent_job=absent\nsystem_daemon_plist=absent\n");
  expect(without_path.login_path.empty(), "a probe without login_path still parses");
  expect_worker_error(WorkerErrorCode::command,
                      [] { (void)parse_probe_output("arch=arm64\n"); },
                      "a truncated probe is an error");
}

void test_write_file_and_removal_scripts_on_a_scratch_root() {
  TemporaryDirectory scratch("svp-scripts");
  const WorkerLayout layout{.root = scratch.path / "Worker"};
  const fs::path plist = scratch.path / "LaunchAgents" / "org.svp.worker.test.plist";
  // A label nothing is loaded under, so launchctl calls are no-ops.
  const std::string label = "org.svp.worker.test-" + std::to_string(::getpid());

  auto run = [](const std::string& script, const std::string& input = {}) {
    TemporaryDirectory input_dir("svp-scripts-input");
    write_file(input_dir.path / "stdin", input);
    return run_command("/bin/sh -c " + shell_quote(script) + " < " +
                       shell_quote((input_dir.path / "stdin").string()) + " 2>&1");
  };
  expect(run(render_prepare_root_script(layout.root)).first == 0, "prepare root");
  for (const std::string id : {"svpw-aaa", "svpw-bbb"}) {
    const auto [status, output] =
        run(render_write_file_script(layout.pairings() / (id + ".json"), 0600), "{}");
    expect(status == 0, "write pairing: " + output);
  }
  struct stat info{};
  expect(::stat((layout.pairings() / "svpw-aaa.json").c_str(), &info) == 0 &&
             (info.st_mode & 0777) == 0600,
         "pairing file is 0600");
  expect(run(render_prepare_launch_agents_script(plist, layout.root)).first == 0,
         "prepare LaunchAgents directory");
  expect(fs::exists(layout.root / std::string(kCreatedLaunchAgentsMarker)),
         "pairing records that it created the LaunchAgents directory");
  expect(run(render_write_file_script(plist, 0644), "<plist/>").first == 0, "write plist");
  write_file(layout.cas() / "blob", "x");

  auto removal = [&](const std::string& id) {
    return run(render_removal_script(WorkerRemoval{.mode = WorkerServiceMode::user_agent,
                                                   .root = layout.root,
                                                   .plist = plist,
                                                   .pairing_id = id,
                                                   .label = label}));
  };
  const auto first = removal("svpw-aaa");
  expect(first.first == 0 && first.second.find("removed=pairing remaining=1") != std::string::npos,
         "first removal keeps the other pairing: " + first.second);
  expect(fs::exists(layout.root) && fs::exists(plist), "root and plist stay");
  const auto last = removal("svpw-bbb");
  expect(last.first == 0 && last.second.find("removed=all") != std::string::npos,
         "last removal removes everything: " + last.second);
  expect(!fs::exists(layout.root), "root is gone");
  expect(!fs::exists(plist), "plist is gone");
  expect(!fs::exists(plist.parent_path()), "the LaunchAgents directory pairing created is gone");
}

void test_receive_runtime_script_installs_a_verified_runtime() {
  TemporaryDirectory scratch("svp-scripts");
  // A stand-in session program: answers `worker verify-runtime` with success.
  const fs::path program = scratch.path / "program";
  write_file(program, "#!/bin/sh\nexit 0\n");
  fs::permissions(program, fs::perms::owner_all);
  const CoordinatorRuntime runtime = single_program_runtime(program, "test-program", "test");
  stage_runtime_directory(runtime, scratch.path / "stage");
  const WorkerLayout layout{.root = scratch.path / "Worker"};
  const auto [status, output] = run_command(
      "/usr/bin/tar -cf - -C " + shell_quote((scratch.path / "stage").string()) + " . | /bin/sh -c " +
      shell_quote(render_receive_runtime_script(layout.runtimes(), runtime.runtime_id)) + " 2>&1");
  expect(status == 0, "receive script succeeds: " + output);
  expect(output.find("runtime_id=" + svp::exec::blake3_prefixed(runtime.runtime_id)) !=
             std::string::npos,
         "receive script reports the runtime id");
  expect(fs::exists(layout.runtime(runtime.runtime_id) / "bin/svp-builder"),
         "session program installed");
  expect(fs::exists(runtime_manifest_path(layout.runtime(runtime.runtime_id))),
         "manifest installed");
  std::size_t leftovers = 0;
  for (const auto& entry : fs::directory_iterator(layout.runtimes())) {
    leftovers += entry.path().filename().string().starts_with(".incoming") ? 1 : 0;
  }
  expect(leftovers == 0, "no staging directory left behind");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec-worker-launchd-job-tests",
      {
          {"agent plist lints and runs the runtime", test_agent_plist_lints_and_runs_the_runtime},
          {"daemon plist runs as the worker user", test_daemon_plist_runs_as_the_worker_user},
          {"scripts parse", test_scripts_parse},
          {"shell quote survives the shell", test_shell_quote_survives_the_shell},
          {"probe runs here", test_probe_runs_here},
          {"write-file and removal scripts on a scratch root",
           test_write_file_and_removal_scripts_on_a_scratch_root},
          {"receive-runtime script installs a verified runtime",
           test_receive_runtime_script_installs_a_verified_runtime},
      });
}
