#include "journal_test_support.hpp"
#include "scheduler_test_support.hpp"
#include "svp/exec/journal_result_commit_sink.hpp"
#include "svp/exec/journal_scheduler_resume.hpp"
#include "svp/exec/task_attempt_runner.hpp"

#include <algorithm>
#include <set>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kSuite = "svp-exec-journal-result-commit-sink-tests";
// Tasks per lane in these tests: enough for cross-lane dependencies and a
// partial commit to leave work on both sides.
constexpr std::size_t kTasksPerLane = 4;

BuildOutcome run_scheduler(const TaskGraph& graph, Executor& executor, ResultCommitSink& sink,
                           std::span<const CommittedResult> resumed = {},
                           const AttemptObserver& observer = {}) {
  const SteadyClock clock;
  const CancellationToken cancellation;
  std::vector<Executor*> executors{&executor};
  return Scheduler(test_policy(), clock)
      .run(graph, executors, sink, cancellation, observer, resumed);
}

CommittedResult computed_result(ToyRuntime& runtime, const TaskSpec& spec) {
  AttemptOutput output = run_task_attempt(
      runtime.registry, runtime.store, spec,
      AttemptContext{.attempt = 1, .worker_session_id = "ws_direct"}, kNotCancelled);
  return CommittedResult{.result = std::move(output.result),
                         .payloads = std::move(output.payloads),
                         .executor_id = "direct"};
}

std::set<std::string> leased_tasks(const EventLog& log) {
  std::set<std::string> leased;
  for (const AttemptEvent& event : log.events()) {
    if (event.kind == AttemptEventKind::leased) {
      leased.insert(event.task_id);
    }
  }
  return leased;
}

void expect_same_results(const std::vector<CommittedResult>& actual,
                         const std::vector<CommittedResult>& expected, std::string_view what) {
  expect(actual.size() == expected.size(), std::string(what) + ": result count");
  for (const CommittedResult& result : expected) {
    const auto found = std::find_if(actual.begin(), actual.end(), [&](const CommittedResult& other) {
      return other.result.task_id == result.result.task_id;
    });
    expect(found != actual.end(), std::string(what) + ": has " + result.result.task_id);
    expect(found->result.output_digest == result.result.output_digest &&
               found->payloads == result.payloads,
           std::string(what) + ": identical output of " + result.result.task_id);
  }
}

void test_sink_commits_verified_results_to_the_journal() {
  JournalFixture fixture(kSuite);
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(kTasksPerLane));
  RecoveryJournal journal = fixture.create_for_session(kToyBuildSession);
  record_task_graph(journal, graph);

  ToyRuntime runtime;
  InProcessExecutor executor(runtime.registry, runtime.store, {.threads = 2});
  JournalResultCommitSink sink(journal);
  expect_succeeded(run_scheduler(graph, executor, sink), "journaled run");
  expect(sink.committed_count() == graph.size(), "every task committed through the sink");

  InProcessExecutor reference_executor(runtime.registry, runtime.store, {.threads = 2});
  InMemoryResultCommitSink reference;
  expect_succeeded(run_scheduler(graph, reference_executor, reference), "in-memory run");

  const std::vector<CommittedResult> loaded = load_committed_results(journal, graph);
  expect_same_results(loaded, reference.results(), "journal read-back");
  for (const std::string_view lane : {"alpha", "beta"}) {
    expect_equal(reduce_lane(graph, lane, loaded), reduce_lane(graph, lane, reference.results()),
                 "reduction from the journal equals the in-memory reduction");
  }

  const TaskSpec& first = graph.node(0).spec;
  JournalDatabase database(journal.layout().database);
  expect_equal(database.value("SELECT count(*) FROM task WHERE status = 'committed'"),
               std::to_string(graph.size()), "every task row committed");
  expect_equal(database.value("SELECT count(*) FROM artifact"), std::to_string(2 * graph.size()),
               "one output and one result record per task");
  expect_equal(database.value("SELECT cache_key FROM task WHERE task_id = '" + first.task_id + "'"),
               blake3_prefixed(first.cache_key), "cache key recorded on the task row");
  expect_equal(database.value("SELECT outcome, worker_session_id FROM task_attempt WHERE "
                              "task_id = '" + first.task_id + "'"),
               "succeeded\tws_in_process", "attempt row recorded");
  expect_equal(database.value("SELECT processor_id FROM artifact_provenance WHERE artifact_id = '" +
                              journal_result_artifact_id(first.task_id) + "'"),
               "toy.digest@1", "provenance names the task type and version");
}

void test_sink_failure_fails_the_build() {
  JournalFixture fixture(kSuite);
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(1));
  // The journal belongs to another build session: nothing may be committed.
  RecoveryJournal journal = fixture.create_for_session("bs_other");
  ToyRuntime runtime;
  InProcessExecutor executor(runtime.registry, runtime.store, {.threads = 1});
  JournalResultCommitSink sink(journal);
  const BuildOutcome outcome = run_scheduler(graph, executor, sink);
  expect(outcome.failure && outcome.failure->kind == BuildFailureKind::commit_failed,
         "a journal commit failure fails the build");
  expect(sink.committed_count() == 0, "nothing committed");
}

void test_resume_skips_committed_tasks() {
  JournalFixture fixture(kSuite);
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(kTasksPerLane));
  ToyRuntime runtime;
  std::set<std::string> committed_before;
  {
    RecoveryJournal journal = fixture.create_for_session(kToyBuildSession);
    record_task_graph(journal, graph);
    JournalResultCommitSink sink(journal);
    // The first alpha tasks and the beta task that depends on a_000: a
    // dependency-closed prefix, as an interrupted run leaves.
    for (const std::string task_id : {"task.toy.a_000", "task.toy.a_001", "task.toy.b_000"}) {
      const TaskSpec& spec = graph.node(*graph.find(task_id)).spec;
      sink.commit(spec, computed_result(runtime, spec));
      committed_before.insert(task_id);
    }
    // One task was mid-flight when the coordinator stopped.
    journal.set_task_state("task.toy.a_002", TaskState::ready);
    journal.set_task_state("task.toy.a_002", TaskState::leased);
  }

  RecoveryJournal journal = fixture.open();
  const SchedulerResumeState state = resume_scheduler_state(journal, graph, fixture.sources());
  expect(state.committed.size() == committed_before.size(), "committed results resumed");
  expect(journal.task_state("task.toy.a_002") == TaskState::ready, "in-flight task reset");

  InProcessExecutor executor(runtime.registry, runtime.store, {.threads = 2});
  JournalResultCommitSink sink(journal);
  EventLog log;
  const BuildOutcome outcome = run_scheduler(graph, executor, sink, state.committed, log.observer());
  expect_succeeded(outcome, "resumed run");
  expect(outcome.stats.resumed == committed_before.size(), "resumed count");
  expect(outcome.stats.committed == graph.size() - committed_before.size(),
         "only the remaining tasks commit");
  const std::set<std::string> leased = leased_tasks(log);
  for (const std::string& task_id : committed_before) {
    expect(!leased.contains(task_id), task_id + " was not run again");
  }
  expect(leased.size() == graph.size() - committed_before.size(), "every other task ran once");

  InProcessExecutor reference_executor(runtime.registry, runtime.store, {.threads = 2});
  InMemoryResultCommitSink reference;
  expect_succeeded(run_scheduler(graph, reference_executor, reference), "reference run");
  expect_same_results(load_committed_results(journal, graph), reference.results(),
                      "resumed journal");
}

void test_resume_reruns_a_demoted_dependency_only() {
  JournalFixture fixture(kSuite);
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(kTasksPerLane));
  ToyRuntime runtime;
  const std::string dependency = "task.toy.a_000";
  const std::string dependent = "task.toy.b_000";
  fs::path dependency_blob;
  {
    RecoveryJournal journal = fixture.create_for_session(kToyBuildSession);
    record_task_graph(journal, graph);
    JournalResultCommitSink sink(journal);
    for (const std::string& task_id : {dependency, dependent}) {
      const TaskSpec& spec = graph.node(*graph.find(task_id)).spec;
      sink.commit(spec, computed_result(runtime, spec));
    }
    const JournalCommittedTask committed = *journal.committed_task(dependency);
    dependency_blob = committed.artifacts.front().path;
  }
  // The dependency's output is damaged on disk; resume must not trust it.
  write_file(dependency_blob, std::string(fs::file_size(dependency_blob), 'x'));

  RecoveryJournal journal = fixture.open();
  const SchedulerResumeState state = resume_scheduler_state(journal, graph, fixture.sources());
  expect(state.report.demoted.size() == 1 && state.report.demoted.front().task_id == dependency,
         "the damaged dependency is demoted");
  expect(state.committed.size() == 1 && state.committed.front().result.task_id == dependent,
         "its intact dependent stays committed");

  InProcessExecutor executor(runtime.registry, runtime.store, {.threads = 2});
  JournalResultCommitSink sink(journal);
  EventLog log;
  expect_succeeded(run_scheduler(graph, executor, sink, state.committed, log.observer()),
                   "resumed run");
  const std::set<std::string> leased = leased_tasks(log);
  expect(leased.contains(dependency), "the demoted dependency runs again");
  expect(!leased.contains(dependent), "the committed dependent is not re-run");
  expect(load_committed_results(journal, graph).size() == graph.size(), "all tasks committed");
}

void test_resume_rejects_a_different_graph() {
  JournalFixture fixture(kSuite);
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(kTasksPerLane));
  {
    RecoveryJournal journal = fixture.create_for_session(kToyBuildSession);
    record_task_graph(journal, graph);
  }
  std::vector<TaskNode> nodes;
  for (std::size_t index = 0; index < graph.size(); ++index) {
    nodes.push_back(graph.node(index));
  }
  nodes.front().spec.cache_key = blake3_digest("different work");
  const TaskGraph changed(std::move(nodes));
  RecoveryJournal journal = fixture.open();
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(resume_scheduler_state(journal, changed,
                                                                   fixture.sources())); },
                    "a changed cache key is not resumed");

  const TaskGraph smaller = make_toy_graph(two_lane_toy_tasks(kTasksPerLane - 1));
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(resume_scheduler_state(journal, smaller,
                                                                   fixture.sources())); },
                    "journaled tasks missing from the graph are not resumed");
}

void test_scheduler_rejects_invalid_resumed_results() {
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(1));
  ToyRuntime runtime;
  const CommittedResult good = computed_result(runtime, graph.node(0).spec);
  InProcessExecutor executor(runtime.registry, runtime.store, {.threads = 1});
  InMemoryResultCommitSink sink;

  CommittedResult corrupt = good;
  corrupt.payloads.front().front() ^= std::byte{0x01};
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(run_scheduler(graph, executor, sink,
                                                          std::span(&corrupt, 1))); },
                    "corrupt resumed payload");
  CommittedResult foreign = good;
  foreign.result.task_id = "task.toy.elsewhere";
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(run_scheduler(graph, executor, sink,
                                                          std::span(&foreign, 1))); },
                    "resumed task outside the graph");
  const std::vector<CommittedResult> twice{good, good};
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(run_scheduler(graph, executor, sink, twice)); },
                    "task resumed twice");

  // Everything resumed: nothing to run, and the build still succeeds.
  std::vector<CommittedResult> all;
  for (std::size_t index = 0; index < graph.size(); ++index) {
    all.push_back(computed_result(runtime, graph.node(index).spec));
  }
  const BuildOutcome outcome = run_scheduler(graph, executor, sink, all);
  expect_succeeded(outcome, "fully resumed run");
  expect(outcome.stats.attempts_started == 0 && sink.size() == 0, "nothing re-run or re-sunk");
}

}  // namespace

int main() {
  return run_tests(kSuite,
                   {
                       {"sink commits verified results to the journal",
                        test_sink_commits_verified_results_to_the_journal},
                       {"sink failure fails the build", test_sink_failure_fails_the_build},
                       {"resume skips committed tasks", test_resume_skips_committed_tasks},
                       {"resume reruns a demoted dependency only",
                        test_resume_reruns_a_demoted_dependency_only},
                       {"resume rejects a different graph", test_resume_rejects_a_different_graph},
                       {"scheduler rejects invalid resumed results",
                        test_scheduler_rejects_invalid_resumed_results},
                   });
}
