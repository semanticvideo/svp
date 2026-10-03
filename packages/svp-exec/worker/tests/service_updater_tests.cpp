// The worker service moving itself to a newer runtime (service_updater.hpp):
// the `current` link and its atomic swap, the release order (never a
// downgrade), switching only while no session is live, a failed test-start
// keeping the current runtime, the restart exit status, the real test-start
// of a runtime program, and the install scripts' link.

#include "svp/exec/canonical_json.hpp"
#include "svp/exec/runtime_manifest.hpp"
#include "svp/exec/runtime_release.hpp"
#include "svp/exec/worker/runtime_test_start.hpp"
#include "svp/exec/worker/service_link.hpp"
#include "svp/exec/worker/service_updater.hpp"
#include "svp/exec/worker/worker_scripts.hpp"
#include "worker_test_support.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <vector>

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
using svp::exec::Blake3Digest;
namespace fs = std::filesystem;

// Installs a runtime whose session program is `program_text` under
// <root>/runtimes/<hex>, stamped with `stamp` (unstamped for nullopt).
Blake3Digest install_runtime(const WorkerLayout& layout, std::optional<std::uint64_t> stamp,
                             const std::string& program_text) {
  TemporaryDirectory source("svp-updater-source");
  write_file(source.path / "bin/svp-builder", program_text);
  svp::exec::RuntimeManifest manifest;
  manifest.arch = "arm64";
  manifest.macos_deployment_target = "15.0";
  manifest.files = {svp::exec::describe_runtime_file(source.path, "bin/svp-builder",
                                                     "svp-builder")};
  if (stamp) {
    write_file(source.path / std::string(svp::exec::kRuntimeReleasePath),
               svp::exec::encode_runtime_release_record(*stamp));
    manifest.files.push_back(svp::exec::describe_runtime_file(
        source.path, std::string(svp::exec::kRuntimeReleasePath),
        std::string(svp::exec::kRuntimeReleaseComponent)));
  }
  svp::exec::normalize_runtime_manifest(manifest);
  const Blake3Digest id = svp::exec::compute_runtime_id(manifest);
  const fs::path directory = layout.runtime(id);
  for (const svp::exec::RuntimeManifestFile& file : manifest.files) {
    fs::create_directories((directory / file.path).parent_path());
    fs::copy_file(source.path / file.path, directory / file.path);
  }
  fs::permissions(directory / "bin/svp-builder", fs::perms::owner_all);
  write_file(runtime_manifest_path(directory), svp::exec::encode_runtime_manifest(manifest));
  return id;
}

struct Fixture {
  TemporaryDirectory scratch{"svp-updater"};
  WorkerLayout layout{.root = scratch.path / "Worker"};
  Blake3Digest own{};
  std::vector<Blake3Digest> tested;
  std::set<Blake3Digest> failing;
  int restarts = 0;
  std::vector<std::string> log;

  explicit Fixture(std::optional<std::uint64_t> own_stamp) {
    create_worker_layout(layout);
    own = install_runtime(layout, own_stamp, "own");
    point_current_at(layout, own);
  }

  ServiceUpdater updater(bool launched_through_current = true) {
    return ServiceUpdater(ServiceUpdaterOptions{
        .layout = layout,
        .own_runtime = own,
        .launched_through_current = launched_through_current,
        .test_start =
            [this](const fs::path& directory, const Blake3Digest& id, std::uint64_t bytes) {
              expect(directory == layout.runtime(id), "test-start runs the candidate's directory");
              expect(bytes > 0, "test-start is told the runtime's size");
              tested.push_back(id);
              return failing.contains(id) ? TestStartOutcome{false, "refused by the test"}
                                          : TestStartOutcome{true, {}};
            },
        .log = [this](const std::string& line) { log.push_back(line); },
        .request_restart = [this] { ++restarts; }});
  }
};

void test_current_link_swaps_atomically() {
  TemporaryDirectory scratch("svp-updater-link");
  const WorkerLayout layout{.root = scratch.path / "Worker"};
  create_worker_layout(layout);
  expect(!read_current_runtime(layout), "no link yet");
  const Blake3Digest first = install_runtime(layout, 1, "first");
  const Blake3Digest second = install_runtime(layout, 2, "second");
  point_current_at(layout, first);
  expect(read_current_runtime(layout) == first, "current names the first runtime");
  expect_equal(fs::read_symlink(layout.current()).string(),
               "runtimes/" + svp::exec::blake3_hex(first), "the link target is relative");
  expect_equal(read_file(layout.service_program()), std::string("first"),
               "the service program resolves through the link");
  point_current_at(layout, second);
  expect(read_current_runtime(layout) == second, "current names the second runtime");
  expect_equal(read_file(layout.service_program()), std::string("second"),
               "the swapped link resolves to the new runtime");
  struct stat info{};
  expect(::lstat(layout.current().c_str(), &info) == 0 && S_ISLNK(info.st_mode) &&
             info.st_uid == ::getuid(),
         "current is a symlink owned by the worker's user");
  for (const auto& entry : fs::directory_iterator(layout.root)) {
    expect(!entry.path().filename().string().starts_with(".current-"),
           "no temporary link is left behind");
  }
  expect(fs::exists(layout.runtime(first)), "the old runtime is kept");

  fs::remove(layout.current());
  fs::create_symlink("/elsewhere/runtimes/" + svp::exec::blake3_hex(first), layout.current());
  expect(!read_current_runtime(layout), "a link outside runtimes/ names no runtime");
}

void test_launched_through_current() {
  const WorkerLayout layout{.root = "/Library/Application Support/SVP/Worker"};
  expect_equal(layout.service_program().string(),
               std::string("/Library/Application Support/SVP/Worker/current/bin/svp-builder"),
               "the service program path");
  expect(launched_through_current(layout, layout.service_program()),
         "started through <root>/current");
  expect(launched_through_current(
             layout, "/Library/Application Support/SVP/Worker/./current/bin/svp-builder"),
         "the comparison is lexical");
  expect(!launched_through_current(layout, layout.runtime(svp::exec::blake3_digest(
                                               std::string_view("x"))) /
                                               "bin/svp-builder"),
         "a fixed runtime path is not the link");
}

void test_never_downgrades() {
  Fixture fixture(20);
  (void)install_runtime(fixture.layout, 10, "older");
  (void)install_runtime(fixture.layout, std::nullopt, "unstamped");
  ServiceUpdater updater = fixture.updater();
  expect(updater.consider() == UpdateStep::none, "older and unstamped runtimes are no update");
  expect(fixture.tested.empty(), "nothing was test-started");
  expect(read_current_runtime(fixture.layout) == fixture.own, "current is unchanged");
  expect(fixture.restarts == 0, "no restart");
}

void test_switches_to_the_newest_and_restarts() {
  Fixture fixture(10);
  (void)install_runtime(fixture.layout, 20, "newer");
  const Blake3Digest newest = install_runtime(fixture.layout, 30, "newest");
  ServiceUpdater updater = fixture.updater();
  expect(updater.consider() == UpdateStep::switched, "the service switches");
  expect(fixture.tested == std::vector<Blake3Digest>{newest}, "only the newest is test-started");
  expect(read_current_runtime(fixture.layout) == newest, "current names the newest runtime");
  expect(fixture.restarts == 1 && updater.restart_requested(), "a restart was requested once");
  expect(!updater.try_enter_session(), "no session starts after the switch");
  expect(updater.consider() == UpdateStep::busy, "a later check does nothing");
  expect(fixture.restarts == 1, "still one restart");
}

void test_equal_stamps_break_ties_by_runtime_id() {
  Fixture fixture(10);
  const Blake3Digest a = install_runtime(fixture.layout, 20, "tie a");
  const Blake3Digest b = install_runtime(fixture.layout, 20, "tie b");
  ServiceUpdater updater = fixture.updater();
  expect(updater.consider() == UpdateStep::switched, "the service switches");
  expect(read_current_runtime(fixture.layout) == std::max(a, b),
         "the larger runtime_id wins a tie, as on every Mac");
}

void test_switches_only_when_idle() {
  Fixture fixture(10);
  ServiceUpdater updater = fixture.updater();
  expect(updater.try_enter_session(), "a session starts");
  const Blake3Digest newer = install_runtime(fixture.layout, 20, "newer");
  updater.runtime_installed();
  expect(updater.consider() == UpdateStep::waiting_for_idle, "a live session holds the switch");
  expect(fixture.tested.empty(), "nothing is test-started while a session is live");
  expect(read_current_runtime(fixture.layout) == fixture.own, "current is unchanged");
  expect(updater.try_enter_session(), "a second session starts");
  updater.leave_session();
  expect(read_current_runtime(fixture.layout) == fixture.own, "one session is still live");
  updater.leave_session();
  expect(read_current_runtime(fixture.layout) == newer,
         "the last session's end switches the service");
  expect(fixture.restarts == 1, "a restart was requested");
  expect(updater.live_sessions() == 0, "no session is live");
}

void test_failed_test_start_keeps_the_current_runtime() {
  Fixture fixture(10);
  const Blake3Digest broken = install_runtime(fixture.layout, 20, "broken");
  fixture.failing.insert(broken);
  ServiceUpdater updater = fixture.updater();
  expect(updater.consider() == UpdateStep::failed, "the only candidate fails its test-start");
  expect(read_current_runtime(fixture.layout) == fixture.own, "current is unchanged");
  expect(fixture.restarts == 0 && !updater.restart_requested(), "no restart");
  expect(updater.try_enter_session(), "sessions start again after a failed test-start");
  updater.leave_session();
  expect(fixture.tested.size() == 1, "a failed runtime is not test-started again");
  bool logged = false;
  for (const std::string& line : fixture.log) {
    logged = logged || line.find("refused by the test") != std::string::npos;
  }
  expect(logged, "the log says why the runtime was not adopted");

  const Blake3Digest good = install_runtime(fixture.layout, 15, "good");
  expect(updater.consider() == UpdateStep::switched, "a working newer runtime is adopted");
  expect(read_current_runtime(fixture.layout) == good, "current names the working runtime");
}

void test_next_candidate_after_a_failure() {
  Fixture fixture(10);
  const Blake3Digest fallback = install_runtime(fixture.layout, 20, "fallback");
  const Blake3Digest broken = install_runtime(fixture.layout, 30, "broken");
  fixture.failing.insert(broken);
  ServiceUpdater updater = fixture.updater();
  expect(updater.consider() == UpdateStep::switched, "the next newest is tried");
  expect(fixture.tested == std::vector<Blake3Digest>{broken, fallback}, "newest first");
  expect(read_current_runtime(fixture.layout) == fallback, "current names the fallback");
}

void test_damaged_candidate_is_not_adopted() {
  Fixture fixture(10);
  const Blake3Digest damaged = install_runtime(fixture.layout, 20, "damaged");
  write_file(fixture.layout.runtime(damaged) / "bin/svp-builder", "tampered");
  const Blake3Digest restamped = install_runtime(fixture.layout, 30, "restamped");
  // A release file edited after install no longer matches the manifest, so
  // it orders nothing.
  write_file(fixture.layout.runtime(restamped) / std::string(svp::exec::kRuntimeReleasePath),
             svp::exec::encode_runtime_release_record(40));
  ServiceUpdater updater = fixture.updater();
  expect(updater.consider() == UpdateStep::failed, "a runtime that does not verify fails");
  expect(fixture.tested.empty(), "it is never started");
  expect(read_current_runtime(fixture.layout) == fixture.own, "current is unchanged");
}

void test_reports_its_state_for_hello_ack() {
  Fixture fixture(10);
  const Blake3Digest broken = install_runtime(fixture.layout, 20, "broken");
  fixture.failing.insert(broken);
  ServiceUpdater updater = fixture.updater();
  ServiceUpdateState state = updater.state();
  expect(state.self_update && state.release_stamp == std::optional<std::uint64_t>(10) &&
             state.declined_runtimes.empty(),
         "a self-updating service reports its release");
  expect(state.pending && state.pending->runtime_id == broken &&
             state.pending->release_stamp == 20 && state.pending->bytes > 0,
         "the runtime it will try next is reported pending");
  expect(updater.consider() == UpdateStep::failed, "the candidate fails");
  state = updater.state();
  expect(state.declined_runtimes == std::vector<Blake3Digest>{broken},
         "a runtime whose test-start failed is reported as declined");
  expect(!state.pending, "nothing is pending after it was declined");

  const Blake3Digest good = install_runtime(fixture.layout, 15, "good");
  expect(updater.try_enter_session(), "a session holds the switch");
  expect(updater.state().pending && updater.state().pending->runtime_id == good,
         "a committed switch is reported while sessions are live");
  updater.leave_session();
  expect(updater.state().pending && updater.state().pending->runtime_id == good,
         "and while switching to it");
  ServiceUpdater off = fixture.updater(/*launched_through_current=*/false);
  expect(!off.state().self_update, "a service not started through current does not update");
}

void test_disabled_without_the_current_link() {
  Fixture fixture(10);
  (void)install_runtime(fixture.layout, 20, "newer");
  ServiceUpdater updater = fixture.updater(/*launched_through_current=*/false);
  expect(!updater.enabled(), "updating is off");
  expect(updater.disabled_reason().find("current") != std::string::npos,
         "the reason names the link: " + updater.disabled_reason());
  expect(updater.consider() == UpdateStep::disabled, "nothing happens");
  expect(read_current_runtime(fixture.layout) == fixture.own, "current is unchanged");
}

void test_restart_exit_code() {
  expect(kWorkerRestartExitCode == EX_TEMPFAIL, "the restart status is EX_TEMPFAIL");
  expect(kWorkerRestartExitCode != 0, "launchd's KeepAlive restarts a nonzero exit");
  expect(kWorkerRestartExitCode != 1 && kWorkerRestartExitCode != 2,
         "distinct from svp-builder's failure and usage statuses");
}

void test_real_test_start() {
  TemporaryDirectory scratch("svp-updater-start");
  const Blake3Digest id = svp::exec::blake3_digest(std::string_view("runtime"));
  const auto program = [&](const std::string& name, const std::string& body) {
    const fs::path directory = scratch.path / name;
    write_file(directory / "bin/svp-builder", "#!/bin/sh\n" + body);
    fs::permissions(directory / "bin/svp-builder", fs::perms::owner_all);
    return directory;
  };
  // Arguments: worker verify-runtime --runtime-dir <dir> --expect <id>.
  const fs::path good = program("good", "[ \"$2\" = verify-runtime ] || exit 9\n"
                                        "echo \"runtime_id=$6\"\n");
  expect(test_start_runtime(good, id, std::chrono::seconds(10)).passed,
         "a program that verifies passes");
  const fs::path failing = program("failing", "exit 1\n");
  const TestStartOutcome failed = test_start_runtime(failing, id, std::chrono::seconds(10));
  expect(!failed.passed && failed.reason.find("status 1") != std::string::npos,
         "a failing program fails: " + failed.reason);
  const fs::path wrong = program("wrong", "echo runtime_id=b3:00\n");
  expect(!test_start_runtime(wrong, id, std::chrono::seconds(10)).passed,
         "a program reporting another runtime fails");
  const fs::path hung = program("hung", "exec sleep 30\n");
  const TestStartOutcome timed_out = test_start_runtime(hung, id, std::chrono::milliseconds(300));
  expect(!timed_out.passed && timed_out.reason.find("within") != std::string::npos,
         "a hung program is killed at the deadline: " + timed_out.reason);
  expect(!test_start_runtime(scratch.path / "missing", id, std::chrono::seconds(1)).passed,
         "a missing program fails");

  expect(test_start_deadline(0) == std::chrono::milliseconds(kTestStartLaunchAllowance),
         "an empty runtime gets the launch allowance");
  expect(test_start_deadline(kTestStartMinVerifyBytesPerSecond * 4) ==
             std::chrono::milliseconds(kTestStartLaunchAllowance) + std::chrono::seconds(4),
         "the deadline grows with the runtime's bytes");
}

void test_point_current_script_on_a_scratch_root() {
  TemporaryDirectory scratch("svp-updater-script");
  const WorkerLayout layout{.root = scratch.path / "it's a root"};
  create_worker_layout(layout);
  const Blake3Digest first = install_runtime(layout, 1, "#!/bin/sh\n");
  const Blake3Digest second = install_runtime(layout, 2, "#!/bin/sh\n");
  const auto run = [&](const Blake3Digest& id) {
    return run_command("/bin/sh -c " + shell_quote(render_point_current_script(layout.root, id)) +
                       " 2>&1");
  };
  const auto [status, output] = run(first);
  expect(status == 0, "the script runs: " + output);
  expect(read_current_runtime(layout) == first, "the script creates the link");
  expect_equal(fs::read_symlink(layout.current()).string(),
               "runtimes/" + svp::exec::blake3_hex(first), "with a relative target");
  expect(run(second).first == 0 && read_current_runtime(layout) == first,
         "an existing working link is kept (the service only moves forward itself)");
  fs::remove_all(layout.runtime(first));
  expect(run(second).first == 0 && read_current_runtime(layout) == second,
         "a link whose runtime is gone is replaced");
  for (const auto& entry : fs::directory_iterator(layout.root)) {
    expect(!entry.path().filename().string().starts_with(".current-"),
           "no temporary link is left behind");
  }
}

void test_daemon_install_script_points_current() {
  const std::string script = render_daemon_install_script(DaemonInstall{
      .user = "w",
      .staging = "/Users/w/Library/Caches/org.svp/worker-staging-x",
      .root = default_worker_root(WorkerServiceMode::system_daemon, "/"),
      .plist = launchd_plist_path(WorkerServiceMode::system_daemon, "/"),
      .runtime_id = svp::exec::blake3_digest(std::string_view("runtime"))});
  const std::string target =
      "runtimes/" + svp::exec::blake3_hex(svp::exec::blake3_digest(std::string_view("runtime")));
  expect(script.find("ln -s '" + target + "'") != std::string::npos,
         "the daemon install links current to the runtime, relatively");
  expect(script.find("mv -fh") != std::string::npos, "the link is swapped by rename");
  expect(script.find("chown -h \"$user:$group\" \"$root/current\"") != std::string::npos,
         "the link is owned by the worker's user");
  expect(script.find("$staging/join.json") != std::string::npos,
         "a staged join credential is installed");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec-worker-service-updater-tests",
      {
          {"current link swaps atomically", test_current_link_swaps_atomically},
          {"launched through current", test_launched_through_current},
          {"never downgrades", test_never_downgrades},
          {"switches to the newest and restarts", test_switches_to_the_newest_and_restarts},
          {"equal stamps break ties by runtime_id", test_equal_stamps_break_ties_by_runtime_id},
          {"switches only when idle", test_switches_only_when_idle},
          {"failed test-start keeps the current runtime",
           test_failed_test_start_keeps_the_current_runtime},
          {"next candidate after a failure", test_next_candidate_after_a_failure},
          {"damaged candidate is not adopted", test_damaged_candidate_is_not_adopted},
          {"reports its state for HELLO_ACK", test_reports_its_state_for_hello_ack},
          {"disabled without the current link", test_disabled_without_the_current_link},
          {"restart exit code", test_restart_exit_code},
          {"real test-start", test_real_test_start},
          {"point-current script on a scratch root", test_point_current_script_on_a_scratch_root},
          {"daemon install script points current", test_daemon_install_script_points_current},
      });
}
