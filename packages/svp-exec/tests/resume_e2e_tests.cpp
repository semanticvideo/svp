// End to end: scheduler + in-process and loopback executors + the real
// recovery journal (JournalResultCommitSink) + the real content-addressed
// cache (CasTaskArtifactAccess, in the coordinator and in the worker
// process). A build is killed with SIGKILL mid-run, resumed from its journal,
// and must reduce to exactly the bytes of an uninterrupted build without
// running any committed task again.

#include "journal_test_support.hpp"
#include "scheduler_test_support.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/journal_result_commit_sink.hpp"
#include "svp/exec/journal_scheduler_resume.hpp"
#include "svp/exec/loopback_executor.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <map>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kSuite = "svp-exec-resume-e2e-tests";

// Graph shape: two lanes of this many tasks (see two_lane_toy_tasks), each
// task with its own cached input chunk.
constexpr std::size_t kTasksPerLane = 12;
// Commits the killed build makes before it dies: a third of the graph, so the
// kill lands while leases are outstanding on both executors.
constexpr std::size_t kCommitsBeforeKill = 8;
// Per-task sleep, so several attempts are in flight at any moment.
constexpr std::uint64_t kTaskSleepMs = 20;
// Slots for each executor in this test (a test choice, not a capacity claim).
constexpr std::size_t kInProcessThreads = 2;
constexpr std::size_t kLoopbackSlots = 2;
constexpr std::chrono::milliseconds kWorkerShutdownGrace{500};

std::filesystem::path g_worker_path;

struct Build {
  std::vector<ToyTask> tasks;
  std::map<std::string, std::string> chunk_by_task;
};

std::string chunk_content(const std::string& task_id) {
  return "source chunk for " + task_id + "\n";
}

// Stores every task's input chunk in the cache and builds the graph over it.
Build plan_build(const fs::path& cache_root) {
  Build build;
  CasTaskArtifactAccess planner(expect_ok(CasStore::at(cache_root), "open cache"),
                                "bs_toy.planner");
  build.tasks = two_lane_toy_tasks(kTasksPerLane);
  for (ToyTask& task : build.tasks) {
    const std::string chunk = chunk_content(task.task_id);
    task.inputs = {{"chunk", planner.put(as_byte_span(chunk), "text/plain", "source_chunk")}};
    task.sleep_ms = kTaskSleepMs;
    build.chunk_by_task.emplace(task.task_id, chunk);
  }
  return build;
}

// One coordinator's executors: in-process over its own cache session and a
// loopback worker process over the same cache root.
struct Coordinator {
  TaskTypeRegistry registry;
  CasTaskArtifactAccess artifacts;
  InProcessExecutor in_process;
  LoopbackExecutor loopback;

  Coordinator(const fs::path& cache_root, const std::string& session)
      : artifacts(expect_ok(CasStore::at(cache_root), "open cache"), session),
        in_process(registry, artifacts,
                   {.threads = kInProcessThreads, .worker_session_id = "ws_" + session}),
        loopback(LoopbackExecutorOptions{.executor_id = "loopback",
                                         .worker_executable = g_worker_path,
                                         .worker_arguments = {"--cas-root", cache_root.string()},
                                         .slots = kLoopbackSlots,
                                         .shutdown_grace = kWorkerShutdownGrace}) {
    // The executor reads the registry only once it runs tasks.
    register_toy_tasks(
        registry,
        [this](std::vector<std::byte> bytes, std::string media_type, std::string role) {
          return artifacts.put(bytes, std::move(media_type), std::move(role));
        },
        {});
  }

  BuildOutcome run(const TaskGraph& graph, RecoveryJournal& journal,
                   std::span<const CommittedResult> resumed, const AttemptObserver& observer) {
    JournalResultCommitSink sink(journal);
    const SteadyClock clock;
    const CancellationToken cancellation;
    std::vector<Executor*> executors{&in_process, &loopback};
    return Scheduler(test_policy(), clock)
        .run(graph, executors, sink, cancellation, observer, resumed);
  }
};

std::map<std::string, std::string> reductions(const TaskGraph& graph,
                                              const std::vector<CommittedResult>& results) {
  return {{"alpha", reduce_lane(graph, "alpha", results)},
          {"beta", reduce_lane(graph, "beta", results)}};
}

[[noreturn]] void crash_now() {
  ::kill(::getpid(), SIGKILL);
  ::_exit(3);  // unreachable
}

void test_killed_build_resumes_to_identical_output() {
  TemporaryDirectory scratch(kSuite);
  const fs::path cache_root = scratch.path / "cache";
  const Build build = plan_build(cache_root);
  const TaskGraph graph = make_toy_graph(build.tasks);

  // Reference: one uninterrupted journaled build.
  JournalFixture reference_fixture(kSuite);
  std::map<std::string, std::string> reference;
  std::vector<CommittedResult> reference_results;
  {
    RecoveryJournal journal = reference_fixture.create_for_session(kToyBuildSession);
    record_task_graph(journal, graph);
    Coordinator coordinator(cache_root, "bs_toy.reference");
    expect_succeeded(coordinator.run(graph, journal, {}, {}), "uninterrupted build");
    reference_results = load_committed_results(journal, graph);
    reference = reductions(graph, reference_results);
  }
  for (const ToyTask& task : build.tasks) {
    const auto found = std::find_if(
        reference_results.begin(), reference_results.end(),
        [&](const CommittedResult& result) { return result.result.task_id == task.task_id; });
    expect_equal(to_text(found->payloads.front()),
                 toy_expected_output(task.task_id, task.seed,
                                     {{"chunk", build.chunk_by_task.at(task.task_id)}}),
                 "reference output of " + task.task_id);
  }

  // The interrupted build: a child process that SIGKILLs itself right after
  // its kCommitsBeforeKill-th commit. No executor, cache session, or journal
  // object is alive in this process when it forks.
  JournalFixture fixture(kSuite);
  const pid_t child = ::fork();
  if (child == 0) {
    try {
      RecoveryJournal journal = fixture.create_for_session(kToyBuildSession);
      record_task_graph(journal, graph);
      Coordinator coordinator(cache_root, "bs_toy.killed");
      std::size_t commits = 0;
      static_cast<void>(coordinator.run(graph, journal, {}, [&](const AttemptEvent& event) {
        if (event.kind == AttemptEventKind::committed && ++commits == kCommitsBeforeKill) {
          crash_now();
        }
      }));
    } catch (const std::exception& error) {
      std::fprintf(stderr, "killed build failed early: %s\n", error.what());
      ::_exit(2);
    }
    ::_exit(4);  // finished without being killed
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
         "the build was killed mid-run, not ended early");

  // Resume in a new coordinator.
  RecoveryJournal journal = fixture.open();
  const SchedulerResumeState state = resume_scheduler_state(journal, graph, fixture.sources());
  expect(state.committed.size() == kCommitsBeforeKill,
         "exactly the commits made before the kill survive");
  std::set<std::string> committed_before;
  for (const CommittedResult& committed : state.committed) {
    committed_before.insert(committed.result.task_id);
  }

  EventLog log;
  Coordinator coordinator(cache_root, "bs_toy.resumed");
  const BuildOutcome outcome = coordinator.run(graph, journal, state.committed, log.observer());
  expect_succeeded(outcome, "resumed build");
  expect(outcome.stats.resumed == kCommitsBeforeKill, "resumed results counted");
  expect(outcome.stats.committed == graph.size() - kCommitsBeforeKill,
         "only the remaining tasks commit");
  std::set<std::string> leased;
  for (const AttemptEvent& event : log.events()) {
    if (event.kind == AttemptEventKind::leased) {
      expect(!committed_before.contains(event.task_id),
             event.task_id + " was committed before the kill and must not run again");
      leased.insert(event.task_id);
    }
  }
  expect(leased.size() + committed_before.size() == graph.size(),
         "every task not committed before the kill ran after the resume");

  const std::vector<CommittedResult> resumed_results = load_committed_results(journal, graph);
  expect(resumed_results.size() == graph.size(), "every task committed in the journal");
  expect(reductions(graph, resumed_results) == reference,
         "reduced output is byte-identical to the uninterrupted build");
  for (const CommittedResult& expected : reference_results) {
    const auto found = std::find_if(resumed_results.begin(), resumed_results.end(),
                                    [&](const CommittedResult& result) {
                                      return result.result.task_id == expected.result.task_id;
                                    });
    expect(found->payloads == expected.payloads &&
               found->result.output_digest == expected.result.output_digest,
           "identical committed bytes for " + expected.result.task_id);
  }

  const JournalFinish finish = journal.finish_success(JournalRetention::delete_on_success);
  expect(!finish.retained && !fs::exists(finish.journal_root), "journal removed after success");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <svp-exec-test-worker>\n", argv[0]);
    return 2;
  }
  g_worker_path = argv[1];
  return run_tests(kSuite, {{"killed build resumes to identical output",
                             test_killed_build_resumes_to_identical_output}});
}
